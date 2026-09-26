// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// TCP over DPDK: the same role split as --io tcp (sender listens, receiver
// connects), but neither the kernel nor its socket layer is on the path. The
// NIC is a DPDK PMD behind vfio-pci -- the same port setup the raw-datagram
// DPDK backend uses (dpdk_port.h) -- and the TCP state machine is lwIP,
// linked into our process and driven from our poll loop (lwip_tcp.h).
//
// The division of labour, which is the whole point of the exercise:
//   lwIP  : connection setup, sequence numbers, ACK/window management,
//           retransmission, checksums, ARP -- an off-the-shelf, widely
//           deployed implementation of the protocol. We wrote none of it.
//   here  : the frame transport (rte_mbuf in, rte_mbuf out), the poll cadence
//           and the record framing that turns the byte stream back into the
//           datagrams the caller of the Backend interface expects.
#include "dgram_io/backend.h"

#if defined(HAVE_DPDK) && defined(HAVE_LWIP)

#include <rte_ethdev.h>
#include <rte_mbuf.h>

#include <cstring>
#include <string>

#include "dpdk_port.h"
#include "lwip_tcp.h"
#include "dgram_io/limits.h"

namespace dgram_io {

namespace {

constexpr int kTxBatch = 32;   // frames staged before a burst
constexpr int kRxBurst = 64;   // must match the stack's poll burst
// Minimum Ethernet frame without FCS. The kernel pads runts on transmit; a
// PMD does not, and the peer NIC drops what arrives short. This matters far
// more here than on the UDP path: a pure TCP ACK is 54 bytes and an ARP
// request 42, so without padding the connection would never even open --
// and the failure would look like a dead link, not like a missing memset.
constexpr uint32_t kMinFrame = 60;
// How long queue() will keep the sender inside the backend waiting for window
// to free up before it gives up on a record. Each spin polls the NIC once, so
// this is milliseconds -- far more than a full send buffer (TCP_SND_BUF, 1 MB)
// takes to drain at 10G -- and a refusal means the peer has stopped reading
// rather than merely being slow.
constexpr int kTxSpins = 20000;

class DpdkFrameIo final : public LwipFrameIo {
 public:
  dpdkport::Port port;

  ~DpdkFrameIo() override {
    for (int i = 0; i < q_; ++i) rte_pktmbuf_free(txq_[i]);
    port.teardown();
  }

  bool tx_frame(const uint8_t* data, uint32_t len) override {
    rte_mbuf* m = rte_pktmbuf_alloc(port.pool);
    if (!m) {
      tx_kick();  // reclaim: the pool is drained by frames still in flight
      m = rte_pktmbuf_alloc(port.pool);
      if (!m) {
        ++alloc_fail;
        return false;
      }
    }
    uint8_t* p = rte_pktmbuf_mtod(m, uint8_t*);
    std::memcpy(p, data, len);
    uint32_t flen = len;
    if (flen < kMinFrame) {  // see kMinFrame: ACKs and ARP live down here
      std::memset(p + flen, 0, kMinFrame - flen);
      flen = kMinFrame;
      ++padded;
    }
    m->data_len = static_cast<uint16_t>(flen);
    m->pkt_len = flen;
    txq_[q_] = m;
    if (++q_ == kTxBatch) tx_kick();
    return true;
  }

  void tx_kick() override {
    int off = 0;
    while (off < q_) {
      const uint16_t sent = rte_eth_tx_burst(port.id, 0, txq_ + off,
                                             static_cast<uint16_t>(q_ - off));
      off += sent;
      if (sent == 0) {
        if (++spins_ < 100000) continue;  // same policy as the dpdk backend
        spins_ = 0;
        for (; off < q_; ++off) {
          rte_pktmbuf_free(txq_[off]);
          ++tx_drop;
        }
        break;
      }
      frames_out += sent;
      spins_ = 0;
    }
    q_ = 0;
  }

  int rx_frames(int max, void (*deliver)(void*, const uint8_t*, uint32_t),
                void* ctx) override {
    if (max > kRxBurst) max = kRxBurst;
    rte_mbuf* bufs[kRxBurst];
    const uint16_t n =
        rte_eth_rx_burst(port.id, 0, bufs, static_cast<uint16_t>(max));
    for (uint16_t i = 0; i < n; ++i) {
      // deliver() copies into a pbuf before returning, so the mbuf's life
      // ends here -- no held[] generation like the raw backend needs, because
      // the stack owns the packet from this point on.
      deliver(ctx, rte_pktmbuf_mtod(bufs[i], const uint8_t*),
              rte_pktmbuf_pkt_len(bufs[i]));
      rte_pktmbuf_free(bufs[i]);
    }
    frames_in += n;
    return n;
  }

  const char* io_name() const override { return "dpdk"; }

  uint64_t frames_in = 0, frames_out = 0, tx_drop = 0, alloc_fail = 0;
  uint64_t padded = 0;

 private:
  rte_mbuf* txq_[kTxBatch];
  int q_ = 0;
  int spins_ = 0;
};

class TcpDpdkBackend final : public Backend {
 public:
  static std::unique_ptr<Backend> create(const Config& cfg, std::string* err);

  bool queue(const void* payload, size_t len) override {
    if (len > dgram_ || stack_.live_peers() == 0) return false;
    for (int spin = 0; spin < kTxSpins; ++spin) {
      if (stack_.send_record(payload, len)) return true;
      // No window. Turning the crank is what frees it: poll pulls in ACKs,
      // flush pushes out whatever is already queued. This is TCP
      // back-pressure reaching the application: if the window stays shut,
      // queue() returns false and the caller's own queue backs up.
      stack_.poll();
      stack_.flush();
    }
    ++tx_refused_;
    return false;
  }

  bool queue_to(const void* payload, size_t len, const Endpoint& to) override {
    if (len > dgram_) return false;
    if (stack_.send_record_to(payload, len, to)) return true;
    ++tx_refused_;
    return false;
  }

  void flush() override {
    // Poll before output: incoming ACKs free send buffer, so the segments we
    // are about to emit go out with the widest window we know about.
    stack_.poll();
    stack_.flush();
  }

  int rx(RxPacket* out, int max) override { return stack_.take(out, max); }

  const char* name() const override { return "tcp-dpdk"; }

  void log_stats(FILE* f) const override {
    stack_.log_stats(f, "io_tcp_dpdk");
    rte_eth_stats st{};
    rte_eth_stats_get(fio_.port.id, &st);
    fprintf(f,
            "io_tcp_dpdk_nic: port=%u frames_in=%llu frames_out=%llu "
            "tx_drop=%llu alloc_fail=%llu padded=%llu tx_refused=%llu "
            "hw_ipackets=%llu hw_opackets=%llu hw_imissed=%llu "
            "hw_ierrors=%llu hw_oerrors=%llu hw_rx_nombuf=%llu\n",
            fio_.port.id, (unsigned long long)fio_.frames_in,
            (unsigned long long)fio_.frames_out,
            (unsigned long long)fio_.tx_drop,
            (unsigned long long)fio_.alloc_fail,
            (unsigned long long)fio_.padded,
            (unsigned long long)tx_refused_, (unsigned long long)st.ipackets,
            (unsigned long long)st.opackets, (unsigned long long)st.imissed,
            (unsigned long long)st.ierrors, (unsigned long long)st.oerrors,
            (unsigned long long)st.rx_nombuf);
  }

 private:
  DpdkFrameIo fio_;
  LwipTcp stack_;
  uint32_t dgram_ = kDefaultDatagram;
  uint64_t tx_refused_ = 0;
};

std::unique_ptr<Backend> TcpDpdkBackend::create(const Config& cfg,
                                                std::string* err) {
  auto b = std::unique_ptr<TcpDpdkBackend>(new TcpDpdkBackend());
  b->dgram_ = cfg.max_datagram;

  dpdkport::Options opt;
  opt.mtu = 1500;         // lwIP's netif MTU; segments are MSS-bounded anyway
  opt.max_frame = 1514;
  opt.pool_name = "mb_tcp";
  opt.tag = "io_tcp_dpdk";
  if (!dpdkport::setup(cfg, opt, &b->fio_.port, err)) return nullptr;

  LwipParams p;
  std::memcpy(p.mac, b->fio_.port.mac, 6);
  p.ip = cfg.dpdk_ip;
  p.peer_ip = cfg.dst_ip;
  p.port = cfg.port;
  p.server = !cfg.listener;  // the sender is the server (see tcp_backend.cpp)
  p.peers = cfg.tcp_peers < 1 ? 1 : cfg.tcp_peers;
  p.max_record = b->dgram_;
  p.setup_ms = cfg.tcp_setup_ms;
  p.nodelay = cfg.tcp_nodelay;
  if (p.ip.empty()) {
    *err = "tcp-dpdk needs dpdk_ip, our IPv4";
    return nullptr;
  }
  if (!p.server && p.peer_ip.empty()) {
    *err = "tcp-dpdk client (listener) needs dst_ip, the server's address";
    return nullptr;
  }
  if (!b->stack_.init(p, &b->fio_, err)) return nullptr;
  if (!b->stack_.wait_ready(err)) return nullptr;
  return b;
}

}  // namespace

std::unique_ptr<Backend> make_tcp_dpdk_backend(const Config& cfg,
                                               std::string* err) {
  return TcpDpdkBackend::create(cfg, err);
}

}  // namespace dgram_io

#else  // !(HAVE_DPDK && HAVE_LWIP)

#include <string>

namespace dgram_io {
std::unique_ptr<Backend> make_tcp_dpdk_backend(const Config&, std::string* err) {
#ifndef HAVE_DPDK
  *err = "built without DPDK support (install DPDK's development package, "
         "libdpdk in pkg-config, and rebuild)";
#else
  *err =
      "built without the lwIP TCP stack: run scripts/get_lwip.sh to fetch it, "
      "then rebuild (the Makefile detects third_party/lwip)";
#endif
  return nullptr;
}
}  // namespace dgram_io

#endif

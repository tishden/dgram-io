// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// DPDK backend: the NIC is taken over by its poll-mode driver (behind
// vfio-pci) and polled from userspace -- no syscalls, no kernel code and no
// XDP machinery on the datapath. EAL is initialised inside create() on the
// core the process is already pinned to; one port, one RX + one TX queue.
//
// EAL is process-wide: one DPDK backend (dpdk or tcp-dpdk) per process, and
// once it is destroyed EAL is cleaned up and cannot be initialised again.
//
// The mbuf lifecycle implements the Backend RX contract directly: packets
// returned by rx() are views into mbufs which are freed at the start of the
// *next* rx() call. TX copies the payload into a fresh mbuf (same policy as
// the other backends) and bursts on flush().
//
// Unlike AF_XDP in zero-copy mode, where a frame the driver cannot place is
// dropped without a trace, PMD drop accounting is complete: anything the
// caller did not receive shows up in rte_eth_stats (imissed/ierrors/
// rx_nombuf), printed in log_stats().
#include "dgram_io/backend.h"

#ifdef HAVE_DPDK

#include <arpa/inet.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dpdk_port.h"
#include "dgram_io/pkt.h"
#include "dgram_io/limits.h"

namespace dgram_io {

namespace {

constexpr int kTxBatch = 16;   // mirror the other backends
constexpr int kRxBatch = 256;  // drain hard; imissed counts what we miss

class DpdkBackend final : public Backend {
 public:
  static std::unique_ptr<Backend> create(const Config& cfg, std::string* err);
  ~DpdkBackend() override {
    // Free anything still held (rx views + unsent tx queue) before the port
    // and its pool go away.
    for (uint32_t i = 0; i < held_n_; ++i) rte_pktmbuf_free(held_[i]);
    for (int i = 0; i < q_; ++i) rte_pktmbuf_free(txq_[i]);
    port_.teardown();
  }

  bool queue(const void* payload, size_t len) override {
    if (!have_dst_) return false;
    return queue_frame(tmpl_, payload, len);
  }

  bool queue_to(const void* payload, size_t len, const Endpoint& to) override {
    return queue_frame(pkt::with_dst(tmpl_, to), payload, len);
  }

  void flush() override {
    int off = 0;
    while (off < q_) {
      const uint16_t sent = rte_eth_tx_burst(
          port_.id, 0, txq_ + off, static_cast<uint16_t>(q_ - off));
      off += sent;
      // TX ring full: keep offering for a bounded spin, then drop. A wedged
      // NIC (link flap) must not freeze the caller inside flush().
      if (sent == 0) {
        if (++spins_ < 100000) continue;
        spins_ = 0;
        for (; off < q_; ++off) {
          rte_pktmbuf_free(txq_[off]);
          ++tx_drop_;
        }
        break;
      }
      spins_ = 0;
    }
    q_ = 0;
  }

  int rx(RxPacket* out, int max) override {
    // Free the mbufs handed out last call: their views die now.
    if (held_n_ > 0) {
      rte_pktmbuf_free_bulk(held_, held_n_);
      held_n_ = 0;
    }
    if (max > kRxBatch) max = kRxBatch;
    rte_mbuf* bufs[kRxBatch];
    const uint16_t n =
        rte_eth_rx_burst(port_.id, 0, bufs, static_cast<uint16_t>(max));
    int emitted = 0;
    for (uint16_t i = 0; i < n; ++i) {
      held_[held_n_++] = bufs[i];
      // A peer that resolves our MAC itself gets no answer otherwise. With
      // every destination MAC configured explicitly (as on AWS) this never
      // comes up; a peer with its own IP stack -- a hardware UDP endpoint,
      // say -- simply ARPs, gets silence, and transmits nothing at all. From
      // the outside that is indistinguishable from a card that does not work.
      if (arp_reply(bufs[i])) continue;
      pkt::View v;
      if (pkt::parse(rte_pktmbuf_mtod(bufs[i], const uint8_t*),
                     rte_pktmbuf_pkt_len(bufs[i]), &v) &&
          v.dst_port_be == tmpl_.src_port_be) {
        out[emitted].data = v.payload;
        out[emitted].len = v.len;
        out[emitted].from = v.from;
        ++emitted;
        ++rx_pkts_;
      } else {
        ++rx_filtered_;  // no BPF filter here: we see every frame on the port
      }
    }
    return emitted;
  }

  const char* name() const override { return "dpdk"; }

  void log_stats(FILE* f) const override {
    rte_eth_stats st{};
    rte_eth_stats_get(port_.id, &st);
    fprintf(f,
            "io_dpdk: port=%u tx_pkts=%llu tx_drop=%llu tx_refused=%llu "
            "rx_pkts=%llu "
            "rx_filtered=%llu hw_ipackets=%llu hw_opackets=%llu "
            "hw_imissed=%llu hw_ierrors=%llu hw_oerrors=%llu "
            "arp_replies=%llu hw_rx_nombuf=%llu\n",
            port_.id, (unsigned long long)tx_pkts_, (unsigned long long)tx_drop_,
            (unsigned long long)tx_refused_, (unsigned long long)rx_pkts_, (unsigned long long)rx_filtered_,
            (unsigned long long)st.ipackets, (unsigned long long)st.opackets,
            (unsigned long long)st.imissed, (unsigned long long)st.ierrors,
            (unsigned long long)st.oerrors, (unsigned long long)arp_replies_,
            (unsigned long long)st.rx_nombuf);
  }

 private:
  bool queue_frame(const pkt::Template& t, const void* payload, size_t len) {
    if (len > dgram_) return false;
    rte_mbuf* m = rte_pktmbuf_alloc(port_.pool);
    if (!m) {  // pool exhausted: reclaim by bursting what we hold
      flush();
      m = rte_pktmbuf_alloc(port_.pool);
      if (!m) {
        ++tx_refused_;  // back-pressure: the caller offers it again
        return false;
      }
    }
    uint8_t* data = rte_pktmbuf_mtod(m, uint8_t*);
    const size_t flen = pkt::build(data, t, ip_id_++, payload, len);
    m->data_len = static_cast<uint16_t>(flen);
    m->pkt_len = static_cast<uint32_t>(flen);
    txq_[q_] = m;
    ++tx_pkts_;
    if (++q_ == kTxBatch) flush();
    return true;
  }

  dpdkport::Port port_;
  // Answers "who has <our ip>" with our MAC, in place, on the frame we were
  // handed. Requests for anything else are left alone -- this is a courtesy
  // to the peer, not a general-purpose stack: no cache, no gratuitous
  // announcements, no replies to replies.
  bool arp_reply(rte_mbuf* m) {
    uint8_t* f = rte_pktmbuf_mtod(m, uint8_t*);
    const uint32_t len = rte_pktmbuf_pkt_len(m);
    if (len < 42 || f[12] != 0x08 || f[13] != 0x06) return false;   // not ARP
    if (f[20] != 0x00 || f[21] != 0x01) return false;               // not a request
    uint32_t target;
    std::memcpy(&target, f + 38, 4);                                // TPA
    if (target != tmpl_.src_ip_be) return false;                    // not for us
    uint8_t sha[6], spa[4];
    std::memcpy(sha, f + 22, 6);
    std::memcpy(spa, f + 28, 4);
    std::memcpy(f, sha, 6);                       // eth dst = requester
    std::memcpy(f + 6, tmpl_.src_mac, 6);         // eth src = us
    f[21] = 0x02;                                 // opcode: reply
    std::memcpy(f + 22, tmpl_.src_mac, 6);        // SHA = us
    std::memcpy(f + 28, &tmpl_.src_ip_be, 4);     // SPA = us
    std::memcpy(f + 32, sha, 6);                  // THA = requester
    std::memcpy(f + 38, spa, 4);                  // TPA = requester
    m->pkt_len = m->data_len = 42;
    if (rte_eth_tx_burst(port_.id, 0, &m, 1) == 1) {
      ++arp_replies_;
      --held_n_;      // handed to the NIC; it frees the mbuf on completion
      return true;
    }
    return false;     // could not send: leave it to the normal free path
  }

  uint32_t dgram_ = kDefaultDatagram;
  uint64_t arp_replies_ = 0;
  pkt::Template tmpl_{};
  bool have_dst_ = false;
  uint16_t ip_id_ = 0;
  rte_mbuf* txq_[kTxBatch];
  int q_ = 0;
  int spins_ = 0;
  rte_mbuf* held_[kRxBatch];
  uint32_t held_n_ = 0;
  uint64_t tx_pkts_ = 0, tx_drop_ = 0, tx_refused_ = 0, rx_pkts_ = 0,
           rx_filtered_ = 0;
};

std::unique_ptr<Backend> DpdkBackend::create(const Config& cfg,
                                             std::string* err) {
  auto b = std::unique_ptr<DpdkBackend>(new DpdkBackend());
  b->dgram_ = cfg.max_datagram;
  dpdkport::Options opt;
  opt.mtu = pkt::kIpLen + pkt::kUdpLen + b->dgram_;
  opt.max_frame = pkt::kHdrLen + b->dgram_;
  opt.pool_name = "mb";
  opt.tag = "io_dpdk";
  if (!dpdkport::setup(cfg, opt, &b->port_, err)) return nullptr;

  // Our identity: MAC from the port, IP from the flag (no kernel netdev).
  std::memcpy(b->tmpl_.src_mac, b->port_.mac, 6);
  if (cfg.dpdk_ip.empty() ||
      inet_pton(AF_INET, cfg.dpdk_ip.c_str(), &b->tmpl_.src_ip_be) != 1) {
    *err = "dpdk needs dpdk_ip, our IPv4 for the headers we build";
    return nullptr;
  }
  b->tmpl_.src_port_be = htons(cfg.port);
  b->tmpl_.dst_port_be = htons(cfg.port);

  if (!cfg.listener) {
    if (inet_pton(AF_INET, cfg.dst_ip.c_str(), &b->tmpl_.dst_ip_be) != 1) {
      *err = "bad dst_ip " + cfg.dst_ip;
      return nullptr;
    }
    const bool mcast = pkt::is_mcast(b->tmpl_.dst_ip_be);
    if (mcast) {
      pkt::mcast_mac(b->tmpl_.dst_ip_be, b->tmpl_.dst_mac);
    } else if (cfg.dst_mac.empty()) {
      *err = "dpdk needs dst_mac, the peer's MAC: there is no kernel under "
             "the port and so no ARP (read it before binding to vfio-pci)";
      return nullptr;
    } else if (!pkt::parse_mac(cfg.dst_mac.c_str(), b->tmpl_.dst_mac)) {
      *err = "bad dst_mac " + cfg.dst_mac;
      return nullptr;
    }
    b->have_dst_ = true;
  }
  return b;
}

}  // namespace

std::unique_ptr<Backend> make_dpdk_backend(const Config& cfg,
                                           std::string* err) {
  return DpdkBackend::create(cfg, err);
}

}  // namespace dgram_io

#else  // !HAVE_DPDK

namespace dgram_io {
std::unique_ptr<Backend> make_dpdk_backend(const Config&, std::string* err) {
  *err =
      "built without DPDK support (install DPDK's development package and "
      "rebuild: the Makefile detects it via pkg-config libdpdk)";
  return nullptr;
}
}  // namespace dgram_io

#endif

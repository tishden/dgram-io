// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// TCP over AF_XDP: the third TCP datapath. Same role split as --io tcp (sender listens, receiver connects) and the same
// off-the-shelf stack as --io tcp-dpdk (lwIP, lwip_tcp.h) -- only the
// frame transport underneath changes, from a DPDK PMD to an XSK on a NIC
// queue.
//
// That is the point of the exercise: with the stack and the framing held
// fixed, the difference between --io tcp-dpdk and --io tcp-xdp is exactly
// what the kernel's XSK path costs versus owning the NIC outright.
//
// Two things AF_XDP forces that DPDK does not:
//   * the XDP program has to redirect TCP *and ARP* into the socket (the
//     userspace stack does its own ARP), and it has to match on either port
//     because the client's local port is ephemeral -- see
//     xdp_tcp_filter.bpf.c;
//   * frames must be padded to 60 bytes by us. Pure ACKs are 54 bytes and
//     the XSK TX path does not pad, so without it the connection would not
//     open at all. xsk::Port::tx_submit does the padding for both backends.
#include "dgram_io/backend.h"

#if defined(HAVE_XDP) && defined(HAVE_LWIP)

#include <arpa/inet.h>

#include <cstring>
#include <string>

#include "dgram_io/limits.h"
#include "dgram_io/pkt.h"
#include "lwip_tcp.h"
#include "xdp_socket.h"

namespace dgram_io {

namespace {

// How long queue() keeps the sender inside the backend waiting for send
// window before it gives up on a record. Same rationale as the DPDK TCP
// backend: far more than a full send buffer takes to drain, so a refusal
// means the peer has stopped reading rather than merely being slow.
constexpr int kTxSpins = 20000;

std::string ip_to_string(uint32_t be) {
  char buf[INET_ADDRSTRLEN] = {0};
  inet_ntop(AF_INET, &be, buf, sizeof(buf));
  return buf;
}

class XdpFrameIo final : public LwipFrameIo {
 public:
  xsk::Port port;

  bool tx_frame(const uint8_t* data, uint32_t len) override {
    uint64_t addr = 0;
    uint8_t* buf = port.tx_alloc(&addr);
    if (!buf) {  // TX frames all in flight and the NIC is not completing them
      ++alloc_fail;
      return false;
    }
    std::memcpy(buf, data, len);
    return port.tx_submit(addr, len);  // pads runts (ACKs, ARP) itself
  }

  void tx_kick() override { port.tx_kick_if_pending(); }

  int rx_frames(int max, void (*deliver)(void*, const uint8_t*, uint32_t),
                void* ctx) override {
    // The previous round's frames are dead: deliver() copies each one into a
    // pbuf before returning, so nothing outlives the call.
    port.rx_recycle();
    return port.rx_peek(max, deliver, ctx);
  }

  const char* io_name() const override { return "xdp"; }

  uint64_t alloc_fail = 0;
};

class TcpXdpBackend final : public Backend {
 public:
  static std::unique_ptr<Backend> create(const Config& cfg, std::string* err);

  bool queue(const void* payload, size_t len) override {
    if (len > dgram_ || stack_.live_peers() == 0) return false;
    for (int spin = 0; spin < kTxSpins; ++spin) {
      if (stack_.send_record(payload, len)) return true;
      stack_.poll();   // ACKs free send buffer
      stack_.flush();  // and push what is already queued
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
    // Poll before output, and poll here at all: a sender that never calls
    // rx() gets no other chance to take ACKs in. Without it the window
    // closes and the stream stalls.
    stack_.poll();
    stack_.flush();
  }

  int rx(RxPacket* out, int max) override { return stack_.take(out, max); }

  const char* name() const override { return "tcp-xdp"; }

  void log_stats(FILE* f) const override {
    stack_.log_stats(f, "io_tcp_xdp");
    char extra[64];
    snprintf(extra, sizeof(extra), "alloc_fail=%llu tx_refused=%llu ",
             (unsigned long long)fio_.alloc_fail,
             (unsigned long long)tx_refused_);
    fio_.port.log_stats(f, "io_tcp_xdp_nic", extra);
  }

 private:
  XdpFrameIo fio_;
  LwipTcp stack_;
  uint32_t dgram_ = kDefaultDatagram;
  uint64_t tx_refused_ = 0;
};

std::unique_ptr<Backend> TcpXdpBackend::create(const Config& cfg,
                                               std::string* err) {
  // Frames here are bounded by the MSS, not by the record size, but the check
  // stays identical to the datagram XDP backend's so the two fail alike: a
  // record that could not travel as one datagram is refused here too.
  if (cfg.max_datagram > xsk::kMaxRxFrame - pkt::kHdrLen) {
    *err = "tcp-xdp cannot carry a " + std::to_string(cfg.max_datagram) +
           "-byte datagram: an RX buffer holds " +
           std::to_string(xsk::kMaxRxFrame) + " bytes";
    return nullptr;
  }
  auto b = std::unique_ptr<TcpXdpBackend>(new TcpXdpBackend());
  b->dgram_ = cfg.max_datagram;

  xsk::Options opt;
  opt.ifname = cfg.ifname;
  opt.queue = cfg.xdp_queue;
  opt.force_copy = cfg.force_copy;
  opt.bpf_obj = cfg.bpf_obj;
  opt.default_obj = "xdp_tcp_filter.bpf.o";  // TCP + ARP, either port
  opt.port = cfg.port;
  opt.tag = "io_tcp_xdp";
  if (!b->fio_.port.open(opt, err)) return nullptr;

  LwipParams p;
  std::memcpy(p.mac, b->fio_.port.mac(), 6);
  p.ip = ip_to_string(b->fio_.port.ip_be());
  p.netmask = ip_to_string(b->fio_.port.netmask_be());
  p.peer_ip = cfg.dst_ip;
  p.port = cfg.port;
  p.server = !cfg.listener;  // the sender is the server (see tcp_backend.cpp)
  p.peers = cfg.tcp_peers < 1 ? 1 : cfg.tcp_peers;
  p.max_record = b->dgram_;
  p.setup_ms = cfg.tcp_setup_ms;
  p.nodelay = cfg.tcp_nodelay;
  if (!p.server && p.peer_ip.empty()) {
    *err = "tcp-xdp client (listener) needs dst_ip, the server's address";
    return nullptr;
  }
  if (!b->stack_.init(p, &b->fio_, err)) return nullptr;
  if (!b->stack_.wait_ready(err)) return nullptr;
  return b;
}

}  // namespace

std::unique_ptr<Backend> make_tcp_xdp_backend(const Config& cfg,
                                              std::string* err) {
  return TcpXdpBackend::create(cfg, err);
}

}  // namespace dgram_io

#else  // !(HAVE_XDP && HAVE_LWIP)

#include <string>

namespace dgram_io {
std::unique_ptr<Backend> make_tcp_xdp_backend(const Config&, std::string* err) {
#ifndef HAVE_XDP
  *err = "built without AF_XDP support (install libxdp's development "
         "package, libxdp in pkg-config, and rebuild)";
#else
  *err =
      "built without the lwIP TCP stack: run scripts/get_lwip.sh to fetch it, "
      "then rebuild (the Makefile detects third_party/lwip)";
#endif
  return nullptr;
}
}  // namespace dgram_io

#endif

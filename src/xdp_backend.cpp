// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// AF_XDP backend: an XSK socket bound to one NIC queue, kernel network stack
// bypassed. The custom XDP program (xdp_filter.bpf.c)
// redirects only our UDP port into the socket and XDP_PASSes everything else,
// so ARP/ICMP/ssh keep working and a concurrent kernel-UDP benchmark on
// another port is unaffected.
//
// The XSK itself (UMEM geometry, filter attach, fill/completion bookkeeping,
// TX frame allocation, carrier wait) lives in io/xdp_socket.h, shared with
// the TCP-over-XDP backend. What is left here is what a frame contains: the
// header template, ARP resolution of the peer, and the parse/port filter on
// receive.
#include "dgram_io/backend.h"

#ifdef HAVE_XDP

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "dgram_io/pkt.h"
#include "dgram_io/limits.h"
#include "dgram_io/xdp_socket.h"

namespace dgram_io {

namespace {

constexpr int kTxBatch = 16;  // mirror the sendmmsg batch

// Kernel ARP cache lookup ("ip" in dotted quad). The XDP filter passes ARP
// through to the kernel, so resolution keeps working while we run.
bool arp_read(const std::string& ip, uint8_t mac[6]) {
  std::ifstream f("/proc/net/arp");
  std::string line;
  std::getline(f, line);  // header
  while (std::getline(f, line)) {
    std::istringstream is(line);
    std::string addr, hw_type, flags, hw;
    is >> addr >> hw_type >> flags >> hw;
    if (addr == ip && flags != "0x0" && pkt::parse_mac(hw.c_str(), mac)) return true;
  }
  return false;
}

// The cache entry may have been GC'ed since setup primed it. Actively force
// neighbour resolution: a throwaway UDP datagram to the discard port makes
// the kernel ARP for the address (the peer's XDP filter passes both ARP and
// non-benchmark UDP ports through to its kernel).
bool arp_lookup(const std::string& ip, uint8_t mac[6]) {
  if (arp_read(ip, mac)) return true;
  const int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s >= 0) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(9);  // discard
    inet_pton(AF_INET, ip.c_str(), &sa.sin_addr);
    const char b = 0;
    sendto(s, &b, 1, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&sa),
           sizeof(sa));
    close(s);
  }
  for (int i = 0; i < 50; ++i) {  // <= 500 ms
    if (arp_read(ip, mac)) return true;
    usleep(10 * 1000);
  }
  return false;
}

class XdpBackend final : public Backend {
 public:
  static std::unique_ptr<Backend> create(const Config& cfg, std::string* err);

  bool queue(const void* payload, size_t len) override {
    if (!have_dst_) return false;
    return queue_frame(tmpl_, payload, len);
  }

  bool queue_to(const void* payload, size_t len, const Endpoint& to) override {
    return queue_frame(pkt::with_dst(tmpl_, to), payload, len);
  }

  void flush() override {
    port_.tx_kick_if_pending();
    batch_ = 0;
  }

  int rx(RxPacket* out, int max) override {
    port_.rx_recycle();  // the frames handed out last call die now
    Sink s{this, out, 0, max};
    port_.rx_peek(max, &XdpBackend::on_frame, &s);
    return s.emitted;
  }

  const char* name() const override { return "xdp"; }

  void log_stats(FILE* f) const override {
    char extra[64];
    snprintf(extra, sizeof(extra), "rx_filtered=%llu ",
             (unsigned long long)rx_filtered_);
    port_.log_stats(f, "io_xdp", extra);
  }

 private:
  struct Sink {
    XdpBackend* self;
    RxPacket* out;
    int emitted;
    int max;
  };

  static void on_frame(void* ctx, const uint8_t* frame, uint32_t len) {
    Sink* s = static_cast<Sink*>(ctx);
    pkt::View v;
    if (s->emitted < s->max && pkt::parse(frame, len, &v) &&
        v.dst_port_be == s->self->tmpl_.src_port_be) {
      s->out[s->emitted].data = v.payload;
      s->out[s->emitted].len = v.len;
      s->out[s->emitted].from = v.from;
      ++s->emitted;
    } else {
      ++s->self->rx_filtered_;  // stray traffic the BPF filter let through
    }
  }

  bool queue_frame(const pkt::Template& t, const void* payload, size_t len) {
    if (len > dgram_) return false;
    uint64_t addr = 0;
    uint8_t* buf = port_.tx_alloc(&addr);
    if (!buf) return false;
    const size_t flen = pkt::build(buf, t, ip_id_++, payload, len);
    if (!port_.tx_submit(addr, static_cast<uint32_t>(flen))) return false;
    if (++batch_ >= kTxBatch) flush();
    return true;
  }

  xsk::Port port_;
  // Payload budget one UMEM frame leaves after L2/L3/L4 headers. Fixed by the
  // umem geometry -- a jumbo datagram does not fit an AF_XDP frame at all,
  // and setup refuses it rather than truncating.
  uint32_t dgram_ = kDefaultDatagram;
  pkt::Template tmpl_{};
  bool have_dst_ = false;
  uint16_t ip_id_ = 0;
  int batch_ = 0;
  uint64_t rx_filtered_ = 0;
};

std::unique_ptr<Backend> XdpBackend::create(const Config& cfg,
                                            std::string* err) {
  auto b = std::unique_ptr<XdpBackend>(new XdpBackend());
  // Checked before anything else: headers plus payload must live inside one
  // UMEM frame (there is no multi-buffer path here), and 2048-byte frames
  // leave room for a standard MTU and nothing like jumbo. Diagnosing this
  // after an unrelated ARP or BPF failure would be needlessly confusing.
  const uint32_t hdr_room = static_cast<uint32_t>(pkt::kHdrLen);
  if (cfg.max_datagram + hdr_room > xsk::kFrameSize) {
    *err = "--io xdp cannot carry a " + std::to_string(cfg.max_datagram) +
           "-byte datagram: the UMEM frame is " +
           std::to_string(xsk::kFrameSize) + " bytes (headers take " +
           std::to_string(hdr_room) + ")";
    return nullptr;
  }
  b->dgram_ = cfg.max_datagram;

  xsk::Options opt;
  opt.ifname = cfg.ifname;
  opt.queue = cfg.queue;
  opt.force_copy = cfg.force_copy;
  opt.bpf_obj = cfg.bpf_obj;
  opt.port = cfg.port;
  opt.tag = "io_xdp";
  if (!b->port_.open(opt, err)) return nullptr;

  std::memcpy(b->tmpl_.src_mac, b->port_.mac(), 6);
  b->tmpl_.src_ip_be = b->port_.ip_be();
  b->tmpl_.src_port_be = htons(cfg.port);
  b->tmpl_.dst_port_be = htons(cfg.port);  // symmetric: one port, one filter

  if (!cfg.listener) {
    if (inet_pton(AF_INET, cfg.dst_ip.c_str(), &b->tmpl_.dst_ip_be) != 1) {
      *err = "bad dst " + cfg.dst_ip;
      return nullptr;
    }
    const bool mcast = pkt::is_mcast(b->tmpl_.dst_ip_be);
    if (mcast) {
      pkt::mcast_mac(b->tmpl_.dst_ip_be, b->tmpl_.dst_mac);
    } else if (!cfg.dst_mac.empty()) {
      if (!pkt::parse_mac(cfg.dst_mac.c_str(), b->tmpl_.dst_mac)) {
        *err = "bad --xdp-dst-mac " + cfg.dst_mac;
        return nullptr;
      }
    } else if (!arp_lookup(cfg.dst_ip, b->tmpl_.dst_mac)) {
      *err = "no ARP entry for " + cfg.dst_ip +
             " -- ping it first or pass --dst-mac";
      return nullptr;
    }
    b->have_dst_ = true;
  }
  return b;
}

}  // namespace

std::unique_ptr<Backend> make_xdp_backend(const Config& cfg,
                                          std::string* err) {
  return XdpBackend::create(cfg, err);
}

}  // namespace dgram_io

#else  // !HAVE_XDP

namespace dgram_io {
std::unique_ptr<Backend> make_xdp_backend(const Config&, std::string* err) {
  *err =
      "built without AF_XDP support (install libxdp-devel and rebuild: the "
      "Makefile detects it via pkg-config)";
  return nullptr;
}
}  // namespace dgram_io

#endif

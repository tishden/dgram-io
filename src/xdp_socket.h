// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// Shared AF_XDP plumbing for the two XSK backends: the raw-datagram one
// (xdp_backend.cpp) and the TCP one (tcp_xdp_backend.cpp). Same reason as
// dpdk_port.h: if the two set up their UMEM, rings, filter or link
// wait even slightly differently, an A/B between the stacks quietly turns
// into an A/B between two socket configurations.
//
// What lives here: UMEM geometry, the XDP program attach, the XSK socket,
// the fill/completion bookkeeping, TX frame allocation and the carrier wait.
// What stays in the backends: what a frame *contains*.
//
// Frame lifetime, which is what makes the dgram_io::Backend RX contract work:
// frames handed out by rx_peek() are recycled into the fill ring by the
// *next* rx_recycle(), so a view stays valid for exactly one round.
#pragma once

#ifdef HAVE_XDP

#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <xdp/libxdp.h>
#include <xdp/xsk.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "dgram_io/backend.h"

namespace dgram_io {
namespace xsk {

// RX depth is the whole loss-absorption budget: unlike a kernel UDP socket,
// whose receive buffer (16 MB here, see udp_socket.h) holds tens of
// milliseconds of traffic, an XSK drops on the NIC once the fill ring drains.
// 8192 frames is ~30 ms at 265k pkt/s -- enough to ride out the scheduler
// stalls of a process that is not on an isolated core.
//
// Frame size: 4096, not the 2048 most AF_XDP code uses. In zero-copy mode the
// NIC receives straight into a UMEM chunk minus the kernel's 256-byte
// headroom, and the 82599 (ixgbe) programs that as its RX buffer size in
// whole kilobytes, rounding down: a 2048-byte chunk gives 1792, which becomes
// a 1024-byte buffer. A frame longer than that needs a second descriptor,
// which ixgbe's zero-copy path does not do, so it is dropped -- silently, in
// no counter we can read. Datagrams of 100 bytes never notice; a full-size
// 1514-byte frame is lost every time, retransmissions included, which is how
// it was found (tcp-xdp stalling for good as soon as lwIP coalesced records
// into full segments). 4096 leaves 3840, rounded to 3072 -- a whole standard
// frame on every NIC that rounds this way.
inline constexpr uint32_t kFrameSize = 4096;
inline constexpr uint32_t kXdpHeadroom = 256;  // XDP_PACKET_HEADROOM
// The largest frame that fits one RX buffer however the driver rounds the
// chunk: what the backends check max_datagram against, so an unreceivable
// size fails at setup instead of vanishing on the wire.
inline constexpr uint32_t kMaxRxFrame = (kFrameSize - kXdpHeadroom) / 1024 * 1024;
inline constexpr uint32_t kRxFrames = 8192;                    // fill/RX rings
inline constexpr uint32_t kTxFrames = 2048;                    // TX/completion
inline constexpr uint32_t kNumFrames = kRxFrames + kTxFrames;  // 40 MB UMEM
inline constexpr int kRxBatch = 256;  // drain hard: RX-ring-full inside the ZC
                                      // driver is a silent drop (no counter)
// Minimum Ethernet frame without FCS. The kernel pads runts on transmit; the
// XSK TX path does not, and the peer NIC may silently drop them. TCP's pure
// ACKs (54 bytes) and ARP (42) live entirely below this line.
inline constexpr uint32_t kMinFrame = 60;

struct Options {
  std::string ifname;
  int queue = 0;
  bool force_copy = false;
  std::string bpf_obj;      // empty = default_obj, searched (see open())
  const char* default_obj = "xdp_filter.bpf.o";  // per-backend filter
  uint16_t port = 0;        // written into the filter's port_map
  const char* tag = "io_xdp";
};

// One XSK on one queue of one NIC, plus the filter that feeds it.
class Port {
 public:
  Port() = default;
  Port(const Port&) = delete;  // owns the UMEM mapping and the attach
  Port& operator=(const Port&) = delete;
  ~Port() {
    if (sock_) xsk_socket__delete(sock_);
    if (prog_) {
      xdp_program__detach(prog_, ifindex_, mode_, 0);
      xdp_program__close(prog_);
    }
    if (umem_) xsk_umem__delete(umem_);
    if (area_) munmap(area_, static_cast<size_t>(kNumFrames) * kFrameSize);
  }

  bool open(const Options& opt, std::string* err);

  // --- identity (read off the interface; both backends need it) -----------
  const uint8_t* mac() const { return mac_; }
  uint32_t ip_be() const { return ip_be_; }
  uint32_t netmask_be() const { return netmask_be_; }
  const std::string& ifname() const { return ifname_; }
  bool zerocopy() const { return zerocopy_; }
  bool native() const { return mode_ == XDP_MODE_NATIVE; }

  // --- TX ------------------------------------------------------------------
  // Grabs a free UMEM frame to build into, or nullptr after a bounded spin.
  // Bounded rather than blocking: a wedged NIC (link flap) must not freeze
  // the caller for as long as the link takes to come back.
  uint8_t* tx_alloc(uint64_t* addr) {
    if (tx_free_n_ == 0) {
      for (int spin = 0; tx_free_n_ == 0; ++spin) {
        if (spin >= 100000) {
          ++tx_stall_;
          return nullptr;
        }
        if ((spin & 4095) == 0) kick_tx();
        reap_completions();
      }
    }
    *addr = tx_free_[--tx_free_n_];
    return base_ + *addr;
  }

  void tx_release(uint64_t addr) { tx_free_[tx_free_n_++] = addr; }

  // Submits a built frame. Pads runts (see kMinFrame) -- the caller must have
  // left room, which every frame does inside a kFrameSize UMEM chunk.
  bool tx_submit(uint64_t addr, uint32_t len) {
    if (len < kMinFrame) {
      std::memset(base_ + addr + len, 0, kMinFrame - len);
      len = kMinFrame;
      ++padded_;
    }
    uint32_t idx = 0;
    for (int spin = 0; xsk_ring_prod__reserve(&tx_, 1, &idx) != 1;) {
      if (++spin >= 100000) {
        ++tx_stall_;
        tx_release(addr);
        return false;
      }
      if ((spin & 4095) == 1) kick_tx();
      reap_completions();
    }
    xdp_desc* d = xsk_ring_prod__tx_desc(&tx_, idx);
    d->addr = addr;
    d->len = len;
    d->options = 0;
    xsk_ring_prod__submit(&tx_, 1);
    ++tx_pkts_;
    ++pending_;
    return true;
  }

  // Kick only when something was submitted since the last kick: callers run
  // this every busy-loop pass, and in copy mode need_wakeup stays asserted --
  // unconditional kicks would burn a syscall per iteration.
  void tx_kick_if_pending() {
    if (pending_ > 0) {
      if (xsk_ring_prod__needs_wakeup(&tx_)) kick_tx();
      pending_ = 0;
    }
    reap_completions();
  }

  // --- RX ------------------------------------------------------------------
  // Returns the frames still held from the previous round to the fill ring.
  void rx_recycle() {
    if (held_n_ == 0) return;
    uint32_t idx = 0;
    while (xsk_ring_prod__reserve(&fill_, held_n_, &idx) != held_n_) {
      // Cannot fail persistently: fill depth == kRxFrames and we never hold
      // more frames than we took out of it.
      if (xsk_ring_prod__needs_wakeup(&fill_)) kick_rx();
    }
    for (uint32_t i = 0; i < held_n_; ++i)
      *xsk_ring_prod__fill_addr(&fill_, idx + i) = held_[i];
    xsk_ring_prod__submit(&fill_, held_n_);
    held_n_ = 0;
  }

  // Peeks up to `max` arrived frames; each is handed to sink(ctx, data, len)
  // and remembered for the next rx_recycle(). Returns the count.
  int rx_peek(int max, void (*sink)(void*, const uint8_t*, uint32_t),
              void* ctx) {
    if (max > kRxBatch) max = kRxBatch;
    uint32_t idx = 0;
    const uint32_t n =
        xsk_ring_cons__peek(&rx_, static_cast<uint32_t>(max), &idx);
    if (n == 0) {
      // Empty poll. The fill-ring wakeup matters after we hand the driver new
      // descriptors (rx_recycle); here it is only a safety net for a driver
      // that fell asleep. In copy mode need_wakeup stays asserted, so kicking
      // on *every* empty poll costs one recvfrom per busy-loop pass -- and on
      // the TCP path that is paid by the sender too, which polls for ACKs on
      // every flush. Throttled: at most one kick per kKickEvery empty polls,
      // which in a busy loop is microseconds of extra worst-case wakeup
      // latency for a 64x cut in syscalls.
      if (++empty_polls_ >= kKickEvery) {
        empty_polls_ = 0;
        if (xsk_ring_prod__needs_wakeup(&fill_)) kick_rx();
      }
      return 0;
    }
    empty_polls_ = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const xdp_desc* d = xsk_ring_cons__rx_desc(&rx_, idx + i);
      // The descriptor address includes the kernel's packet headroom offset;
      // recycle the chunk-aligned base.
      held_[held_n_++] = d->addr & ~static_cast<uint64_t>(kFrameSize - 1);
      sink(ctx, base_ + d->addr, d->len);
      ++rx_pkts_;
    }
    xsk_ring_cons__release(&rx_, n);
    return static_cast<int>(n);
  }

  // "<tag>: ..." counters, including the kernel-side XSK drops that are
  // invisible in both NIC and netdev statistics.
  void log_stats(FILE* f, const char* tag, const char* extra) const {
    xdp_statistics xs{};
    socklen_t optlen = sizeof(xs);
    getsockopt(fd_, SOL_XDP, XDP_STATISTICS, &xs, &optlen);
    fprintf(f,
            "%s: if=%s q=%d mode=%s zerocopy=%d tx_pkts=%llu kicks=%llu "
            "tx_stall=%llu padded=%llu rx_pkts=%llu %sxsk_rx_dropped=%llu "
            "xsk_rx_ring_full=%llu xsk_fill_empty=%llu xsk_rx_invalid=%llu\n",
            tag, ifname_.c_str(), queue_, native() ? "native" : "skb",
            zerocopy_ ? 1 : 0, (unsigned long long)tx_pkts_,
            (unsigned long long)kicks_, (unsigned long long)tx_stall_,
            (unsigned long long)padded_, (unsigned long long)rx_pkts_, extra,
            (unsigned long long)xs.rx_dropped,
            (unsigned long long)xs.rx_ring_full,
            (unsigned long long)xs.rx_fill_ring_empty_descs,
            (unsigned long long)xs.rx_invalid_descs);
  }

 private:
  // Detaches an XDP program left on this interface by an earlier instance of
  // ourselves, recognised by program name. Only that program goes: under a
  // libxdp dispatcher every other program stays attached, and a legacy
  // single-program attach is removed only when that one program is ours.
  // Returns true if something was removed.
  bool detach_stale(const char* tag) {
    xdp_multiprog* mp = xdp_multiprog__get_from_ifindex(ifindex_);
    if (!mp || libxdp_get_error(mp)) return false;
    const char* ours = xdp_program__name(prog_);
    auto is_ours = [&](xdp_program* p) {
      const char* n = p ? xdp_program__name(p) : nullptr;
      return n && ours && std::strcmp(n, ours) == 0;
    };
    int r = -1;
    if (xdp_multiprog__is_legacy(mp)) {
      if (is_ours(xdp_multiprog__main_prog(mp))) r = xdp_multiprog__detach(mp);
    } else {
      for (xdp_program* p = xdp_multiprog__next_prog(nullptr, mp); p;
           p = xdp_multiprog__next_prog(p, mp)) {
        if (!is_ours(p)) continue;
        r = xdp_program__detach(p, ifindex_, xdp_multiprog__attach_mode(mp), 0);
        break;
      }
    }
    xdp_multiprog__close(mp);
    if (r == 0)
      fprintf(stderr, "%s: detached a stale '%s' left on %s by an earlier run\n",
              tag, ours, ifname_.c_str());
    return r == 0;
  }

  void kick_tx() {
    sendto(fd_, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
    ++kicks_;
  }
  void kick_rx() {
    recvfrom(fd_, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
    ++kicks_;
  }
  void reap_completions() {
    uint32_t idx = 0;
    const uint32_t n = xsk_ring_cons__peek(&comp_, kTxFrames, &idx);
    for (uint32_t i = 0; i < n; ++i)
      tx_free_[tx_free_n_++] = *xsk_ring_cons__comp_addr(&comp_, idx + i);
    if (n) xsk_ring_cons__release(&comp_, n);
  }

  std::string ifname_;
  int ifindex_ = 0, queue_ = 0, fd_ = -1;
  void* area_ = nullptr;
  uint8_t* base_ = nullptr;
  xsk_umem* umem_ = nullptr;
  xsk_socket* sock_ = nullptr;
  xdp_program* prog_ = nullptr;
  xdp_attach_mode mode_ = XDP_MODE_NATIVE;
  bool zerocopy_ = false;
  xsk_ring_prod fill_{}, tx_{};
  xsk_ring_cons comp_{}, rx_{};
  uint8_t mac_[6] = {0, 0, 0, 0, 0, 0};
  uint32_t ip_be_ = 0, netmask_be_ = 0;
  static constexpr int kKickEvery = 64;  // empty polls between fill kicks
  int empty_polls_ = 0;
  uint32_t pending_ = 0;  // submitted to TX ring, not yet kicked
  uint64_t tx_free_[kTxFrames];
  uint32_t tx_free_n_ = 0;
  uint64_t held_[kRxBatch];
  uint32_t held_n_ = 0;
  uint64_t tx_pkts_ = 0, kicks_ = 0, tx_stall_ = 0, rx_pkts_ = 0, padded_ = 0;
};

inline bool Port::open(const Options& opt, std::string* err) {
  ifname_ = opt.ifname;
  queue_ = opt.queue;
  if (opt.ifname.empty()) {
    *err = "AF_XDP needs an interface (Config::ifname)";
    return false;
  }
  ifindex_ = static_cast<int>(if_nametoindex(opt.ifname.c_str()));
  if (ifindex_ == 0) {
    *err = "unknown interface " + opt.ifname;
    return false;
  }

  // Our own L2/L3 identity comes from the interface itself.
  {
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
      *err = std::string("socket: ") + strerror(errno);
      return false;
    }
    ifreq ifr{};
    std::strncpy(ifr.ifr_name, opt.ifname.c_str(), IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFHWADDR, &ifr) != 0) {
      *err = "SIOCGIFHWADDR " + opt.ifname + ": " + strerror(errno);
      close(s);
      return false;
    }
    std::memcpy(mac_, ifr.ifr_hwaddr.sa_data, 6);
    if (ioctl(s, SIOCGIFADDR, &ifr) != 0) {
      *err = "no IPv4 address on " + opt.ifname + " (needed for our headers)";
      close(s);
      return false;
    }
    ip_be_ = reinterpret_cast<sockaddr_in*>(&ifr.ifr_addr)->sin_addr.s_addr;
    netmask_be_ = htonl(0xffffff00);  // sane default if the ioctl fails
    if (ioctl(s, SIOCGIFNETMASK, &ifr) == 0)
      netmask_be_ =
          reinterpret_cast<sockaddr_in*>(&ifr.ifr_netmask)->sin_addr.s_addr;
    close(s);
  }

  // UMEM + rings.
  const size_t umem_size = static_cast<size_t>(kNumFrames) * kFrameSize;
  area_ = mmap(nullptr, umem_size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (area_ == MAP_FAILED) {
    area_ = nullptr;
    *err = std::string("mmap umem: ") + strerror(errno);
    return false;
  }
  base_ = static_cast<uint8_t*>(area_);
  xsk_umem_config ucfg{};
  ucfg.fill_size = kRxFrames;
  ucfg.comp_size = kTxFrames;
  ucfg.frame_size = kFrameSize;
  ucfg.frame_headroom = 0;
  ucfg.flags = 0;
  int ret = xsk_umem__create(&umem_, area_, umem_size, &fill_, &comp_, &ucfg);
  if (ret) {
    *err = std::string("xsk_umem__create: ") + strerror(-ret);
    return false;
  }

  // Load and attach the filter before the socket exists: its redirect falls
  // back to XDP_PASS until the socket lands in the map, so there is no window
  // where traffic is dropped.
  // Default: next to the running binary (a build tree), else where `make
  // install` put it (DGRAM_IO_BPFDIR, set by the Makefile).
  std::string obj = opt.bpf_obj;
  if (obj.empty()) {
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
      exe[n] = 0;
      const std::string dir(exe, exe + n);
      obj = dir.substr(0, dir.find_last_of('/') + 1) + opt.default_obj;
    }
#ifdef DGRAM_IO_BPFDIR
    if (obj.empty() || access(obj.c_str(), R_OK) != 0)
      obj = std::string(DGRAM_IO_BPFDIR) + "/" + opt.default_obj;
#endif
  }
  prog_ = xdp_program__open_file(obj.c_str(), nullptr, nullptr);
  if (libxdp_get_error(prog_)) {
    *err = "xdp_program__open_file " + obj +
           " failed (set Config::bpf_obj to the path of " + opt.default_obj +
           ")";
    prog_ = nullptr;
    return false;
  }
  mode_ = XDP_MODE_NATIVE;
  ret = xdp_program__attach(prog_, ifindex_, mode_, 0);
  if (ret == -EBUSY) {
    // A program is already attached. Often it is *ours*, orphaned by a
    // process that died without running its destructor (SIGKILL): the attach
    // then fails with "Device or resource busy" on every later start and the
    // host looks permanently broken until someone detaches it by hand. Clean
    // up after ourselves -- but only our own program, by name. Anyone else's
    // XDP program is none of our business.
    if (detach_stale(opt.tag)) ret = xdp_program__attach(prog_, ifindex_, mode_, 0);
  }
  if (ret) {  // veth without driver XDP etc.: generic (skb) mode
    mode_ = XDP_MODE_SKB;
    ret = xdp_program__attach(prog_, ifindex_, mode_, 0);
    if (ret == -EBUSY && detach_stale(opt.tag))
      ret = xdp_program__attach(prog_, ifindex_, mode_, 0);
  }
  if (ret) {
    *err = std::string("xdp attach on ") + opt.ifname + ": " + strerror(-ret) +
           " (root required)";
    xdp_program__close(prog_);
    prog_ = nullptr;
    return false;
  }

  xsk_socket_config scfg{};
  scfg.rx_size = kRxFrames;
  scfg.tx_size = kTxFrames;
  scfg.libxdp_flags = XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD;
  scfg.xdp_flags = 0;
  scfg.bind_flags = XDP_USE_NEED_WAKEUP;
  if (!opt.force_copy) {
    scfg.bind_flags |= XDP_ZEROCOPY;
    ret = xsk_socket__create(&sock_, opt.ifname.c_str(),
                             static_cast<uint32_t>(opt.queue), umem_, &rx_,
                             &tx_, &scfg);
    zerocopy_ = ret == 0;
  } else {
    ret = -1;
  }
  if (ret) {  // downgrade: copy mode works everywhere
    scfg.bind_flags = XDP_USE_NEED_WAKEUP | XDP_COPY;
    ret = xsk_socket__create(&sock_, opt.ifname.c_str(),
                             static_cast<uint32_t>(opt.queue), umem_, &rx_,
                             &tx_, &scfg);
  }
  if (ret) {
    *err = std::string("xsk_socket__create on ") + opt.ifname + " queue " +
           std::to_string(opt.queue) + ": " + strerror(-ret);
    return false;
  }
  fd_ = xsk_socket__fd(sock_);

  // Wire the socket and our port into the filter's maps.
  bpf_object* bobj = xdp_program__bpf_obj(prog_);
  const int xsks_fd = bpf_object__find_map_fd_by_name(bobj, "xsks_map");
  const int port_fd = bpf_object__find_map_fd_by_name(bobj, "port_map");
  if (xsks_fd < 0 || port_fd < 0) {
    *err = "filter maps not found in " + obj;
    return false;
  }
  ret = xsk_socket__update_xskmap(sock_, xsks_fd);
  if (ret) {
    *err = std::string("xsk_socket__update_xskmap: ") + strerror(-ret);
    return false;
  }
  const uint32_t key = 0;
  const uint32_t port_be = htons(opt.port);
  bpf_map_update_elem(port_fd, &key, &port_be, BPF_ANY);

  // Populate the fill ring with the whole RX half; TX half -> free list.
  uint32_t idx;
  if (xsk_ring_prod__reserve(&fill_, kRxFrames, &idx) != kRxFrames) {
    *err = "fill ring reserve failed";
    return false;
  }
  for (uint32_t i = 0; i < kRxFrames; ++i)
    *xsk_ring_prod__fill_addr(&fill_, idx + i) =
        static_cast<uint64_t>(i) * kFrameSize;
  xsk_ring_prod__submit(&fill_, kRxFrames);
  if (xsk_ring_prod__needs_wakeup(&fill_)) kick_rx();
  for (uint32_t i = kRxFrames; i < kNumFrames; ++i)
    tx_free_[tx_free_n_++] = static_cast<uint64_t>(i) * kFrameSize;

  // Binding an XSK can make the driver reset the device (ixgbe logs
  // "Multiqueue Disabled"), and the link retrains on BOTH ends of the wire
  // for a second or two; anything transmitted before carrier returns just
  // evaporates. Wait it out (instant on veth and on settled links).
  {
    const std::string carrier = "/sys/class/net/" + opt.ifname + "/carrier";
    bool up = false;
    for (int i = 0; i < 200 && !up; ++i) {  // <= 20 s
      std::ifstream c(carrier);
      int v = 0;
      up = static_cast<bool>(c >> v) && v == 1;
      if (!up) usleep(100 * 1000);
    }
    if (!up)
      fprintf(stderr, "%s: warning: no carrier on %s after 20 s\n", opt.tag,
              opt.ifname.c_str());
    else
      usleep(300 * 1000);  // settle: the peer may still be retraining
  }

  fprintf(stderr, "%s: %s q=%d %s %s filter=%s\n", opt.tag,
          opt.ifname.c_str(), opt.queue, native() ? "native" : "skb",
          zerocopy_ ? "zerocopy" : "copy", obj.c_str());
  return true;
}

}  // namespace xsk
}  // namespace dgram_io

#endif  // HAVE_XDP

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// Kernel-UDP backend: the step-1..6 socket path moved behind dgram_io::Backend,
// byte-for-byte. Sender role: connected socket (or unconnected + explicit
// address for multicast), TX batched into sendmmsg groups of 16. Receiver
// role: bound socket, optional group join, recvmmsg batches of 32.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

#include <vector>

#include "dgram_io/backend.h"
#include "dgram_io/pkt.h"
#include "dgram_io/limits.h"

namespace dgram_io {

namespace {

class UdpBackend final : public Backend {
 public:
  static std::unique_ptr<Backend> create(const Config& cfg, std::string* err);
  ~UdpBackend() override {
    if (fd_ >= 0) close(fd_);
  }

  bool queue(const void* payload, size_t len) override {
    return enqueue(payload, len, connected_ ? nullptr : &dst_);
  }

  bool queue_to(const void* payload, size_t len, const Endpoint& to) override {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = to.ip_be;
    sa.sin_port = to.port_be;
    return enqueue(payload, len, &sa);
  }

  void flush() override {
    int off = 0;
    while (off < q_) {
      const int r = sendmmsg(fd_, mm_ + off, q_ - off, 0);
      if (r < 0) {
        ++send_errs_;
        break;
      }
      ++syscalls_;
      off += r;
    }
    q_ = 0;
  }

  int rx(RxPacket* out, int max) override {
    if (max > kRxBatch) max = kRxBatch;
    for (int i = 0; i < max; ++i) {  // recvmmsg rewrites namelen each call
      rmm_[i].msg_hdr.msg_name = &rnames_[i];
      rmm_[i].msg_hdr.msg_namelen = sizeof(rnames_[i]);
    }
    const int r = recvmmsg(fd_, rmm_, max, MSG_DONTWAIT, nullptr);
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
      return -1;
    }
    for (int i = 0; i < r; ++i) {
      // A datagram larger than the buffer comes back cut, with MSG_TRUNC set
      // and msg_len reporting only what was stored. Passing it up would look
      // exactly like a corrupt packet and get quietly repaired by NACK, so it
      // is counted and dropped instead -- a misconfigured MTU has to show up
      // as itself.
      if (rmm_[i].msg_hdr.msg_flags & MSG_TRUNC) {
        ++truncated_;
        out[i].data = nullptr;
        out[i].len = 0;
        continue;
      }
      out[i].data = rxbuf(i);
      out[i].len = rmm_[i].msg_len;
      out[i].from = Endpoint{};
      out[i].from.ip_be = rnames_[i].sin_addr.s_addr;
      out[i].from.port_be = rnames_[i].sin_port;
    }
    return r;
  }

  const char* name() const override { return "udp"; }

  void log_stats(FILE* f) const override {
    fprintf(f, "io_udp: syscalls=%llu send_errs=%llu rx_truncated=%llu\n",
            (unsigned long long)syscalls_, (unsigned long long)send_errs_,
            (unsigned long long)truncated_);
  }

 private:
  static constexpr int kBatch = 16;   // datagrams per sendmmsg
  static constexpr int kRxBatch = 32; // datagrams per recvmmsg

  bool enqueue(const void* payload, size_t len, const sockaddr_in* to) {
    if (len > dgram_) return false;
    std::memcpy(txbuf(q_), payload, len);
    iov_[q_] = {txbuf(q_), len};
    std::memset(&mm_[q_], 0, sizeof(mm_[q_]));
    mm_[q_].msg_hdr.msg_iov = &iov_[q_];
    mm_[q_].msg_hdr.msg_iovlen = 1;
    if (to) {
      to_[q_] = *to;
      mm_[q_].msg_hdr.msg_name = &to_[q_];
      mm_[q_].msg_hdr.msg_namelen = sizeof(to_[q_]);
    }
    if (++q_ == kBatch) flush();
    return true;
  }

  uint8_t* txbuf(int i) { return pool_.data() + static_cast<size_t>(i) * dgram_; }
  uint8_t* rxbuf(int i) { return rbufs_.data() + static_cast<size_t>(i) * dgram_; }

  int fd_ = -1;
  bool connected_ = false;
  sockaddr_in dst_{};
  // Both pools are strided by the datagram size the run asked for: at the
  // 1400 default they are the same 22 KB + 45 KB they always were, and jumbo
  // pays for jumbo only where it is used.
  uint32_t dgram_ = kDefaultDatagram;
  std::vector<uint8_t> pool_;   // kBatch  x dgram_ (TX)
  std::vector<uint8_t> rbufs_;  // kRxBatch x dgram_ (RX)
  sockaddr_in to_[kBatch];
  mmsghdr mm_[kBatch];
  iovec iov_[kBatch];
  int q_ = 0;
  mmsghdr rmm_[kRxBatch];
  iovec riov_[kRxBatch];
  sockaddr_in rnames_[kRxBatch];
  uint64_t syscalls_ = 0, send_errs_ = 0, truncated_ = 0;
};

std::unique_ptr<Backend> UdpBackend::create(const Config& cfg,
                                            std::string* err) {
  auto b = std::unique_ptr<UdpBackend>(new UdpBackend());
  if (cfg.max_datagram == 0 || cfg.max_datagram > kDatagramCap) {
    *err = "max_datagram out of range (1.." +
           std::to_string(kDatagramCap) + ")";
    return nullptr;
  }
  b->dgram_ = cfg.max_datagram;
  b->pool_.resize(static_cast<size_t>(kBatch) * b->dgram_);
  b->rbufs_.resize(static_cast<size_t>(kRxBatch) * b->dgram_);
  b->fd_ = socket(AF_INET, SOCK_DGRAM, 0);
  if (b->fd_ < 0) {
    *err = std::string("socket: ") + strerror(errno);
    return nullptr;
  }
  for (int i = 0; i < kRxBatch; ++i) {
    b->riov_[i] = {b->rxbuf(i), b->dgram_};
    std::memset(&b->rmm_[i], 0, sizeof(b->rmm_[i]));
    b->rmm_[i].msg_hdr.msg_iov = &b->riov_[i];
    b->rmm_[i].msg_hdr.msg_iovlen = 1;
  }

  if (cfg.listener) {
    int rcvbuf = 1 << 24;
    setsockopt(b->fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    const int one = 1;
    setsockopt(b->fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(cfg.port);
    if (bind(b->fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      *err = std::string("bind: ") + strerror(errno);
      return nullptr;
    }
    if (!cfg.group.empty()) {  // multicast fan-out: join the group
      ip_mreq mr{};
      if (inet_pton(AF_INET, cfg.group.c_str(), &mr.imr_multiaddr) != 1) {
        *err = "bad group " + cfg.group;
        return nullptr;
      }
      mr.imr_interface.s_addr = htonl(INADDR_ANY);
      if (!cfg.mcast_if.empty() &&
          inet_pton(AF_INET, cfg.mcast_if.c_str(), &mr.imr_interface) != 1) {
        *err = "bad mcast-if " + cfg.mcast_if;
        return nullptr;
      }
      if (setsockopt(b->fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) !=
          0) {
        *err = std::string("IP_ADD_MEMBERSHIP: ") + strerror(errno);
        return nullptr;
      }
    }
    return b;
  }

  // Sender role.
  int sndbuf = 1 << 22;
  setsockopt(b->fd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  b->dst_.sin_family = AF_INET;
  b->dst_.sin_port = htons(cfg.port);
  if (inet_pton(AF_INET, cfg.dst_ip.c_str(), &b->dst_.sin_addr) != 1) {
    *err = "bad dst " + cfg.dst_ip;
    return nullptr;
  }
  const bool mcast = pkt::is_mcast(b->dst_.sin_addr.s_addr);
  if (mcast) {
    in_addr ifaddr{};
    ifaddr.s_addr = htonl(INADDR_ANY);
    if (!cfg.mcast_if.empty() &&
        inet_pton(AF_INET, cfg.mcast_if.c_str(), &ifaddr) != 1) {
      *err = "bad mcast-if " + cfg.mcast_if;
      return nullptr;
    }
    setsockopt(b->fd_, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr));
    const uint8_t ttl = 1, loop = 1;
    setsockopt(b->fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(b->fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    // Unconnected: NACKs come back from receivers' own unicast addresses.
  } else if (cfg.multi_peer) {
    // Unicast replication fan-out: same reason as multicast above -- a
    // connect()ed socket would make the kernel silently discard NACKs from
    // every receiver except the primary.
  } else {
    if (connect(b->fd_, reinterpret_cast<sockaddr*>(&b->dst_),
                sizeof(b->dst_)) != 0) {
      *err = std::string("connect: ") + strerror(errno);
      return nullptr;
    }
    b->connected_ = true;
  }
  return b;
}

}  // namespace

std::unique_ptr<Backend> make_udp_backend(const Config& cfg, std::string* err) {
  return UdpBackend::create(cfg, err);
}

}  // namespace dgram_io

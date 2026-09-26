// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// Kernel-UDP backend. Sender role: connected socket (or unconnected with an
// explicit address for multicast and multi-peer fan-out), TX batched into
// sendmmsg groups of 16. Receiver role: bound socket, optional group join,
// recvmmsg batches of 32. The socket itself comes from udp_socket.h, shared
// with the io_uring backend.
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

#include <vector>

#include "dgram_io/backend.h"
#include "dgram_io/limits.h"
#include "udp_socket.h"

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
    int n = 0;
    for (int i = 0; i < r; ++i) {
      // A datagram larger than the buffer comes back cut, with MSG_TRUNC set
      // and msg_len reporting only what was stored. Passing it up would look
      // exactly like a corrupt packet and get quietly repaired by whatever
      // recovers loss above, so it is counted and dropped instead -- a
      // misconfigured MTU has to show up as itself.
      if (rmm_[i].msg_hdr.msg_flags & MSG_TRUNC) {
        ++truncated_;
        continue;
      }
      out[n].data = rxbuf(i);
      out[n].len = rmm_[i].msg_len;
      out[n].from = Endpoint{};
      out[n].from.ip_be = rnames_[i].sin_addr.s_addr;
      out[n].from.port_be = rnames_[i].sin_port;
      ++n;
    }
    return n;
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
  // Both pools are strided by Config::max_datagram: 22 KB + 45 KB at the
  // 1400 default, and jumbo pays for jumbo only where it is asked for.
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
  UdpSocket sock;
  if (!open_udp_socket(cfg, &sock, err)) return nullptr;
  b->fd_ = sock.fd;
  b->connected_ = sock.connected;
  b->dst_ = sock.dst;
  for (int i = 0; i < kRxBatch; ++i) {
    b->riov_[i] = {b->rxbuf(i), b->dgram_};
    std::memset(&b->rmm_[i], 0, sizeof(b->rmm_[i]));
    b->rmm_[i].msg_hdr.msg_iov = &b->riov_[i];
    b->rmm_[i].msg_hdr.msg_iovlen = 1;
  }
  return b;
}

}  // namespace

std::unique_ptr<Backend> make_udp_backend(const Config& cfg, std::string* err) {
  return UdpBackend::create(cfg, err);
}

}  // namespace dgram_io

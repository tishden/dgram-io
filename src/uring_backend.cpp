// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// io_uring backend: the kernel UDP socket of the `udp` backend (same options,
// same roles -- src/udp_socket.h), driven through a submission/completion ring
// instead of sendmmsg/recvmmsg. The stack is still the kernel's; what changes
// is how often and how expensively we cross into it.
//
// TX: queue() copies into one of kTxSlots buffers and prepares a SENDMSG SQE;
// flush() is one io_uring_submit for the whole batch. A slot is busy until its
// CQE comes back, so the copy-on-queue contract holds without the caller
// knowing sends complete asynchronously. With cfg.uring_sqpoll a kernel thread
// consumes the SQ and flush() is a store to shared memory, no syscall.
//
// RX: one multishot RECVMSG, armed once, drawing from a provided-buffer ring.
// Each datagram lands in its own buffer and posts a CQE, so an rx() that finds
// nothing is a load from the CQ tail -- no syscall, unlike an idle recvmmsg.
// Buffers handed out by rx() go back to the kernel at the start of the next
// rx(), which is exactly the lifetime the Backend contract promises; giving a
// buffer back is two stores, not a syscall. If every buffer is out, the kernel
// ends the multishot with ENOBUFS (datagrams wait in the socket buffer, they
// are not lost) and the next rx() re-arms it.
//
// Needs: liburing, a kernel with multishot recvmsg (6.0, or a distro backport)
// and io_uring not disabled by the kernel.io_uring_disabled sysctl -- RHEL 9
// ships with it set to 2. No root.
#include "dgram_io/backend.h"

#ifdef HAVE_URING

#include <liburing.h>
#include <sched.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "dgram_io/limits.h"
#include "udp_socket.h"

namespace dgram_io {

namespace {

class UringBackend final : public Backend {
 public:
  static std::unique_ptr<Backend> create(const Config& cfg, std::string* err);
  ~UringBackend() override {
    if (ring_up_) {
      if (br_) io_uring_free_buf_ring(&ring_, br_, kRxBufs, kBufGroup);
      io_uring_queue_exit(&ring_);
    }
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
    if (q_ == 0) return;
    submit();
    q_ = 0;
  }

  int rx(RxPacket* out, int max) override {
    // The views from the previous rx() are dead now: their buffers go back.
    if (n_handed_ > 0) {
      for (int i = 0; i < n_handed_; ++i)
        io_uring_buf_ring_add(br_, rxbuf(handed_[i]), rxstride_, handed_[i],
                              io_uring_buf_ring_mask(kRxBufs), i);
      io_uring_buf_ring_advance(br_, n_handed_);
      n_handed_ = 0;
    }
    reap();
    if (!rx_armed_ && rx_err_ == 0) {
      ++rearms_;
      arm_rx();
      submit();
    }
    if (rx_err_ != 0 && stash_n_ == 0) return -1;
    int n = 0;
    while (n < max && stash_n_ > 0) {
      const Stashed& s = stash_[stash_head_];
      stash_head_ = (stash_head_ + 1) % kRxBufs;
      --stash_n_;
      out[n].data = s.data;
      out[n].len = s.len;
      out[n].from = s.from;
      handed_[n_handed_++] = s.bid;
      ++n;
    }
    return n;
  }

  const char* name() const override { return "uring"; }

  void log_stats(FILE* f) const override {
    fprintf(f,
            "io_uring: submits=%llu send_errs=%llu tx_stalls=%llu "
            "rx_truncated=%llu rx_rearms=%llu rx_nobufs=%llu rx_errs=%llu "
            "sqpoll=%d fixed_file=%d\n",
            (unsigned long long)submits_, (unsigned long long)send_errs_,
            (unsigned long long)tx_stalls_, (unsigned long long)truncated_,
            (unsigned long long)rearms_, (unsigned long long)nobufs_,
            (unsigned long long)rx_errs_, sqpoll_ ? 1 : 0, fixed_ ? 1 : 0);
  }

 private:
  static constexpr int kBatch = 16;      // queued datagrams per auto-submit
  static constexpr int kTxSlots = 256;   // sends in flight at most
  static constexpr int kRxBufs = 256;    // provided buffers (power of two)
  static constexpr unsigned kSqEntries = 256;
  // Every TX slot and every RX buffer can have one CQE outstanding at once;
  // sized so the CQ never overflows into the kernel's slow backlog list.
  static constexpr unsigned kCqEntries = 1024;
  static constexpr int kBufGroup = 0;
  static constexpr uint64_t kRxTag = ~0ull;  // user_data of the multishot

  struct TxSlot {
    msghdr msg;
    iovec iov;
    sockaddr_in to;
  };
  struct Stashed {
    const uint8_t* data;
    uint32_t len;
    uint16_t bid;
    Endpoint from;
  };

  uint8_t* txbuf(int i) { return txpool_.data() + static_cast<size_t>(i) * dgram_; }
  uint8_t* rxbuf(int bid) {
    return rxpool_.data() + static_cast<size_t>(bid) * rxstride_;
  }

  io_uring_sqe* sqe() {
    io_uring_sqe* e = io_uring_get_sqe(&ring_);
    if (!e) {  // SQ full: push what is there, then there is room
      submit();
      e = io_uring_get_sqe(&ring_);
    }
    return e;
  }

  void submit() {
    io_uring_submit(&ring_);
    ++submits_;
  }

  void target(io_uring_sqe* e) {
    if (fixed_) e->flags |= IOSQE_FIXED_FILE;
  }

  void arm_rx() {
    io_uring_sqe* e = sqe();
    io_uring_prep_recvmsg_multishot(e, fixed_ ? 0 : fd_, &rx_msg_, 0);
    target(e);
    e->flags |= IOSQE_BUFFER_SELECT;
    e->buf_group = kBufGroup;
    io_uring_sqe_set_data64(e, kRxTag);
    rx_armed_ = true;
  }

  bool enqueue(const void* payload, size_t len, const sockaddr_in* to) {
    if (len > dgram_) return false;
    if (free_.empty()) {
      reap();
      if (free_.empty()) {
        // Every slot is on the wire or queued behind it. Wait for the kernel
        // rather than drop: back-pressure here is the caller's to see.
        ++tx_stalls_;
        while (free_.empty()) {
          io_uring_submit_and_wait(&ring_, 1);
          ++submits_;
          reap();
        }
      }
    }
    const int i = free_.back();
    free_.pop_back();
    TxSlot& s = slots_[i];
    std::memcpy(txbuf(i), payload, len);
    s.iov = {txbuf(i), len};
    std::memset(&s.msg, 0, sizeof(s.msg));
    s.msg.msg_iov = &s.iov;
    s.msg.msg_iovlen = 1;
    if (to) {
      s.to = *to;
      s.msg.msg_name = &s.to;
      s.msg.msg_namelen = sizeof(s.to);
    }
    io_uring_sqe* e = sqe();
    io_uring_prep_sendmsg(e, fixed_ ? 0 : fd_, &s.msg, 0);
    target(e);
    io_uring_sqe_set_data64(e, static_cast<uint64_t>(i));
    if (++q_ == kBatch) flush();
    return true;
  }

  // Drains the CQ. TX completions free their slot; RX completions are parsed
  // into the stash, which cannot overflow: each holds a distinct buffer.
  void reap() {
    io_uring_cqe* cqes[64];
    for (;;) {
      const unsigned n = io_uring_peek_batch_cqe(&ring_, cqes, 64);
      if (n == 0) return;
      for (unsigned k = 0; k < n; ++k) complete(cqes[k]);
      io_uring_cq_advance(&ring_, n);
    }
  }

  void complete(const io_uring_cqe* c) {
    if (c->user_data != kRxTag) {
      if (c->res < 0) ++send_errs_;
      free_.push_back(static_cast<int>(c->user_data));
      return;
    }
    if (!(c->flags & IORING_CQE_F_MORE)) rx_armed_ = false;
    const bool has_buf = c->flags & IORING_CQE_F_BUFFER;
    const uint16_t bid =
        has_buf ? static_cast<uint16_t>(c->flags >> IORING_CQE_BUFFER_SHIFT) : 0;
    if (c->res < 0) {
      if (c->res == -ENOBUFS) {
        ++nobufs_;
      } else {
        ++rx_errs_;
        // EINVAL on the first arm means this kernel has no multishot recvmsg;
        // re-arming would only repeat it, so rx() starts reporting failure.
        if (c->res == -EINVAL) rx_err_ = c->res;
      }
      if (has_buf) give_back(bid);
      return;
    }
    if (!has_buf) {  // cannot happen with BUFFER_SELECT; do not guess a bid
      ++rx_errs_;
      return;
    }
    uint8_t* buf = rxbuf(bid);
    io_uring_recvmsg_out* o =
        io_uring_recvmsg_validate(buf, c->res, &rx_msg_);
    if (!o) {
      ++rx_errs_;
      give_back(bid);
      return;
    }
    // Same policy as the udp backend: a cut datagram looks like a corrupt one
    // and would be quietly repaired upstream, so it is counted and dropped.
    if (o->flags & MSG_TRUNC) {
      ++truncated_;
      give_back(bid);
      return;
    }
    Stashed& s = stash_[(stash_head_ + stash_n_) % kRxBufs];
    ++stash_n_;
    s.bid = bid;
    s.data = static_cast<const uint8_t*>(io_uring_recvmsg_payload(o, &rx_msg_));
    s.len = o->payloadlen;
    s.from = Endpoint{};
    if (o->namelen >= sizeof(sockaddr_in)) {
      const auto* sa =
          static_cast<const sockaddr_in*>(io_uring_recvmsg_name(o));
      s.from.ip_be = sa->sin_addr.s_addr;
      s.from.port_be = sa->sin_port;
    }
  }

  void give_back(uint16_t bid) {
    io_uring_buf_ring_add(br_, rxbuf(bid), rxstride_, bid,
                          io_uring_buf_ring_mask(kRxBufs), 0);
    io_uring_buf_ring_advance(br_, 1);
  }

  io_uring ring_{};
  bool ring_up_ = false;
  bool sqpoll_ = false;
  bool fixed_ = false;  // fd registered: slot 0, skips an fget per request
  int fd_ = -1;
  bool connected_ = false;
  sockaddr_in dst_{};
  uint32_t dgram_ = kDefaultDatagram;

  std::vector<uint8_t> txpool_;  // kTxSlots x dgram_
  TxSlot slots_[kTxSlots];
  std::vector<int> free_;
  int q_ = 0;

  // Each provided buffer: io_uring_recvmsg_out, the source address, payload.
  io_uring_buf_ring* br_ = nullptr;
  std::vector<uint8_t> rxpool_;  // kRxBufs x rxstride_
  uint32_t rxstride_ = 0;
  msghdr rx_msg_{};  // template: namelen only, no control data
  bool rx_armed_ = false;
  int rx_err_ = 0;
  Stashed stash_[kRxBufs];
  int stash_head_ = 0, stash_n_ = 0;
  uint16_t handed_[kRxBufs];
  int n_handed_ = 0;

  uint64_t submits_ = 0, send_errs_ = 0, tx_stalls_ = 0, truncated_ = 0,
           rearms_ = 0, nobufs_ = 0, rx_errs_ = 0;
};

std::unique_ptr<Backend> UringBackend::create(const Config& cfg,
                                              std::string* err) {
  auto b = std::unique_ptr<UringBackend>(new UringBackend());
  if (cfg.max_datagram == 0 || cfg.max_datagram > kDatagramCap) {
    *err = "max_datagram out of range (1.." +
           std::to_string(kDatagramCap) + ")";
    return nullptr;
  }
  b->dgram_ = cfg.max_datagram;

  UdpSocket sock;
  if (!open_udp_socket(cfg, &sock, err)) return nullptr;
  b->fd_ = sock.fd;
  b->connected_ = sock.connected;
  b->dst_ = sock.dst;

  io_uring_params p{};
  p.flags = IORING_SETUP_CQSIZE;
  p.cq_entries = kCqEntries;
  if (cfg.uring_sqpoll) {
    // The SQ thread inherits our CPU mask. If that is a single CPU, it shares
    // the core with a caller that busy-polls rx() and only runs when the
    // scheduler preempts us: measured on 82599, round trips went from ~30 us
    // to milliseconds. Refuse that here rather than let it pass as a number.
    cpu_set_t mine;
    CPU_ZERO(&mine);
    if (sched_getaffinity(0, sizeof(mine), &mine) == 0 &&
        CPU_COUNT(&mine) == 1 &&
        (cfg.uring_sqpoll_cpu < 0 || CPU_ISSET(cfg.uring_sqpoll_cpu, &mine))) {
      *err = "uring_sqpoll: the caller is pinned to one CPU and the SQ thread "
             "would share it; set uring_sqpoll_cpu to a different CPU";
      return nullptr;
    }
    p.flags |= IORING_SETUP_SQPOLL;
    if (cfg.uring_sqpoll_cpu >= 0) {
      p.flags |= IORING_SETUP_SQ_AFF;
      p.sq_thread_cpu = static_cast<unsigned>(cfg.uring_sqpoll_cpu);
    }
  }
  int r = io_uring_queue_init_params(kSqEntries, &b->ring_, &p);
  if (r < 0) {
    *err = std::string("io_uring_queue_init: ") + strerror(-r);
    if (r == -EPERM)
      *err += " (io_uring is switched off: check sysctl "
              "kernel.io_uring_disabled, RHEL 9 defaults it to 2)";
    return nullptr;
  }
  b->ring_up_ = true;
  b->sqpoll_ = cfg.uring_sqpoll;
  b->fixed_ = io_uring_register_files(&b->ring_, &b->fd_, 1) == 0;

  b->txpool_.resize(static_cast<size_t>(kTxSlots) * b->dgram_);
  b->free_.reserve(kTxSlots);
  for (int i = kTxSlots - 1; i >= 0; --i) b->free_.push_back(i);

  // 64-byte stride keeps every payload cache-line aligned.
  const size_t head = sizeof(io_uring_recvmsg_out) + sizeof(sockaddr_in);
  b->rxstride_ = static_cast<uint32_t>((head + b->dgram_ + 63) & ~size_t{63});
  b->rxpool_.resize(static_cast<size_t>(kRxBufs) * b->rxstride_);
  b->rx_msg_.msg_namelen = sizeof(sockaddr_in);
  b->br_ = io_uring_setup_buf_ring(&b->ring_, kRxBufs, kBufGroup, 0, &r);
  if (!b->br_) {
    *err = std::string("io_uring provided-buffer ring: ") + strerror(-r) +
           " (needs kernel 5.19+)";
    return nullptr;
  }
  for (int i = 0; i < kRxBufs; ++i)
    io_uring_buf_ring_add(b->br_, b->rxbuf(i), b->rxstride_,
                          static_cast<unsigned short>(i),
                          io_uring_buf_ring_mask(kRxBufs), i);
  io_uring_buf_ring_advance(b->br_, kRxBufs);

  // Arm RX now and give the kernel a moment to refuse it: a kernel without
  // multishot recvmsg answers EINVAL, and that has to fail setup here rather
  // than turn into a receiver that silently never receives. Any datagram that
  // lands meanwhile is stashed, not lost.
  b->arm_rx();
  b->submit();
  __kernel_timespec ts{0, 20 * 1000 * 1000};
  io_uring_cqe* c = nullptr;
  if (io_uring_wait_cqe_timeout(&b->ring_, &c, &ts) == 0) b->reap();
  if (b->rx_err_ != 0) {
    *err = std::string("io_uring multishot recvmsg: ") + strerror(-b->rx_err_) +
           " (needs kernel 6.0+ or a backport)";
    return nullptr;
  }
  return b;
}

}  // namespace

std::unique_ptr<Backend> make_uring_backend(const Config& cfg,
                                            std::string* err) {
  return UringBackend::create(cfg, err);
}

}  // namespace dgram_io

#else  // !HAVE_URING

namespace dgram_io {
std::unique_ptr<Backend> make_uring_backend(const Config&, std::string* err) {
  *err =
      "built without io_uring support (install liburing-devel and rebuild: "
      "the Makefile detects it via pkg-config)";
  return nullptr;
}
}  // namespace dgram_io

#endif

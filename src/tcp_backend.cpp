// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// Kernel-TCP backend: the same datagram interface, carried by a reliable
// stream. The **sender is the server** (binds, listens, accepts) and the
// **receiver is the client** (connects, retrying until the server is up):
// one sender fanning out to several receivers is the shape this was built
// for, and it is the side with the fan-out that has to accept. That inversion
// of the usual "listener = receiver" convention is why cfg.listener is not
// what decides the socket role here.
//
// What TCP replaces:
//  * loss: there is none to repair. A layer above that recovers loss itself
//    should switch that off on a stream (is_stream_backend()): it would
//    measure nothing while still changing the send path.
//  * datagram boundaries: gone. Records get a 2-byte length prefix
//    (dgram_io/stream.h) and the reader reassembles them.
//  * a dropped packet: gone as a concept, replaced by *back-pressure*. When
//    the socket buffer fills, send() returns EAGAIN and the unsent bytes stay
//    in our staging buffer; once that is full too, queue() returns false and
//    the caller's own queue is what backs up. A record is never truncated:
//    half a record on the wire would desync the peer for the rest of the
//    connection.
//
// Fan-out: the server accepts cfg.tcp_peers connections and writes the same
// framed stream to each. That is unicast replication with the kernel doing the
// copies, and it is O(N) on the sender -- TCP has no multicast.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "dgram_io/backend.h"
#include "dgram_io/stream.h"
#include "dgram_io/limits.h"

namespace dgram_io {

namespace {

// Staging budget per connection. Deep enough to ride out a receiver stall of
// several milliseconds at 10G line rate, shallow enough that a stuck peer is
// noticed rather than absorbed forever.
constexpr size_t kTxStage = 8u << 20;
// One read() per rx() lands here; sized so a full 10G burst between two polls
// fits without a second syscall.
constexpr size_t kRxBuf = 4u << 20;

uint64_t now_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

void set_nonblock(int fd) {
  const int fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, (fl < 0 ? 0 : fl) | O_NONBLOCK);
}

// Everything that makes a TCP socket behave like a low-latency transport
// rather than a bulk pipe. Applied identically to both ends so an A/B against
// the userspace stack compares stacks, not socket options.
void tune(int fd, const Config& cfg, bool sending) {
  const int one = 1;
  if (cfg.tcp_nodelay)  // Nagle would hold a small record waiting for company
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  // Delayed ACKs on the reading side stall the sender's window at low rates;
  // QUICKACK is a one-shot in Linux but survives long enough to cover the
  // handshake and the first records, and the continuous stream keeps ACKs
  // flowing on their own afterwards.
  setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
  const int buf = 16 << 20;
  if (sending)
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
  else
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
  if (cfg.tcp_busy_poll_us) {
    const int us = static_cast<int>(cfg.tcp_busy_poll_us);
    // Needs CAP_NET_ADMIN above net.core.busy_poll; failure is not fatal,
    // it just means we poll the socket the ordinary way.
    if (setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &us, sizeof(us)) != 0)
      fprintf(stderr, "io_tcp: SO_BUSY_POLL(%d) refused: %s\n", us,
              strerror(errno));
  }
  set_nonblock(fd);
}

// connect() bounded by the setup deadline: a blocking connect to a host that
// drops SYNs would otherwise sit in the kernel's own retry schedule for minutes.
bool connect_until(int fd, const sockaddr_in& sa, uint64_t deadline_ms) {
  set_nonblock(fd);
  if (connect(fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == 0)
    return true;
  if (errno != EINPROGRESS) return false;
  const uint64_t now = now_ms();
  const int wait = now >= deadline_ms ? 0 : static_cast<int>(deadline_ms - now);
  pollfd pf{fd, POLLOUT, 0};
  if (poll(&pf, 1, wait) != 1) {
    errno = ETIMEDOUT;
    return false;
  }
  int so_err = 0;
  socklen_t len = sizeof(so_err);
  getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len);
  errno = so_err;
  return so_err == 0;
}

struct Conn {
  int fd = -1;
  Endpoint peer;
  Framer tx;
  Deframer rx;
  bool up = false;
  bool eof = false;  // FIN seen; closed once the buffered records are out
};

class TcpBackend final : public Backend {
 public:
  static std::unique_ptr<Backend> create(const Config& cfg, std::string* err);
  ~TcpBackend() override {
    for (Conn& c : conns_)
      if (c.fd >= 0) close(c.fd);
    if (listen_fd_ >= 0) close(listen_fd_);
  }

  // Fan-out is all-or-nothing: either every live peer gets the record or none
  // does. false tells the caller to offer the same record again, so a peer
  // that had already taken it would receive it twice.
  bool queue(const void* payload, size_t len) override {
    if (len > dgram_ || live() == 0) return false;
    if (!all_fit(len)) {
      // Some peer's staging is full: push what we have and look once more.
      // Only if a peer is still not draining do we refuse -- and a refusal is
      // a whole record, counted, never a partial one.
      flush();
      if (live() == 0 || !all_fit(len)) {
        ++tx_refused_;
        return false;
      }
    }
    for (Conn& c : conns_) {
      if (!c.up) continue;
      c.tx.append(payload, len);  // cannot fail: all_fit() said so
      ++tx_records_;
    }
    if (staged() >= kFlushAt) flush();
    return true;
  }

  // The reverse-direction path (replies). Over TCP the answer goes
  // back down the same connection the request came from; the endpoint match
  // keeps it honest when several peers are attached.
  bool queue_to(const void* payload, size_t len, const Endpoint& to) override {
    if (len > dgram_) return false;
    for (Conn& c : conns_) {
      if (!c.up) continue;
      if (conns_.size() > 1 &&
          (c.peer.ip_be != to.ip_be || c.peer.port_be != to.port_be))
        continue;
      if (!c.tx.append(payload, len)) {
        ++tx_refused_;
        return false;
      }
      ++tx_records_;
      return true;
    }
    return false;
  }

  void flush() override {
    for (Conn& c : conns_) {
      if (!c.up) continue;
      while (!c.tx.empty()) {
        const ssize_t n = send(c.fd, c.tx.data(), c.tx.size(), MSG_NOSIGNAL);
        if (n > 0) {
          ++syscalls_;
          tx_bytes_ += static_cast<uint64_t>(n);
          if (static_cast<size_t>(n) < c.tx.size()) ++tx_partial_;
          c.tx.consume(static_cast<size_t>(n));
          continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          // Window/socket buffer full. Leave the bytes staged: the next
          // flush() (one sender loop away) retries. This is where TCP's
          // back-pressure starts to reach the caller.
          ++tx_stalls_;
          break;
        }
        if (n < 0 && errno == EINTR) continue;
        ++send_errs_;
        drop_conn(c, "send");
        break;
      }
    }
  }

  int rx(RxPacket* out, int max) override {
    int emitted = 0;
    for (Conn& c : conns_) {
      if (!c.up) continue;
      c.rx.compact();
      // One read per connection per call: the deframer buffer is deep enough
      // that a second syscall would almost always come back EAGAIN, and a
      // syscall on an empty socket costs microseconds.
      if (!c.eof && c.rx.write_space() > 0) {
        const ssize_t n = recv(c.fd, c.rx.write_ptr(), c.rx.write_space(),
                               MSG_DONTWAIT);
        if (n > 0) {
          c.rx.committed(static_cast<size_t>(n));
          rx_bytes_ += static_cast<uint64_t>(n);
          ++syscalls_;
        } else if (n == 0) {
          // The peer is done sending, but what it sent before the FIN is
          // still in the deframer: deliver that first, close after.
          c.eof = true;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
          ++recv_errs_;
          drop_conn(c, "recv");
          continue;
        }
      }
      bool bad = false;
      while (emitted < max) {
        const uint8_t* d;
        uint32_t len;
        if (!c.rx.next(&d, &len, &bad)) break;
        out[emitted].data = d;
        out[emitted].len = len;
        out[emitted].from = c.peer;
        ++emitted;
        ++rx_records_;
      }
      if (bad) {  // impossible unless the peer is a different build
        ++desyncs_;
        drop_conn(c, "framing");
      } else if (c.eof && !c.rx.has_record()) {
        drop_conn(c, "eof");
      }
    }
    // Every connection gone and nothing left to hand out: the datapath is
    // dead, which the caller must be able to tell from "nothing yet".
    if (emitted == 0 && live() == 0) return -1;
    return emitted;
  }

  const char* name() const override { return "tcp"; }

  void log_stats(FILE* f) const override {
    fprintf(f,
            "io_tcp: role=%s conns=%zu live=%d tx_records=%llu tx_bytes=%llu "
            "tx_refused=%llu tx_stalls=%llu tx_partial=%llu rx_records=%llu "
            "rx_bytes=%llu syscalls=%llu send_errs=%llu recv_errs=%llu "
            "desyncs=%llu closed=%llu\n",
            server_ ? "server" : "client", conns_.size(), live(),
            (unsigned long long)tx_records_, (unsigned long long)tx_bytes_,
            (unsigned long long)tx_refused_, (unsigned long long)tx_stalls_,
            (unsigned long long)tx_partial_, (unsigned long long)rx_records_,
            (unsigned long long)rx_bytes_, (unsigned long long)syscalls_,
            (unsigned long long)send_errs_, (unsigned long long)recv_errs_,
            (unsigned long long)desyncs_, (unsigned long long)closed_);
  }

 private:
  // Flush threshold: a caller normally flushes as soon as it has nothing more
  // to send, so this only bounds how much a *saturated* sender stages before
  // it hands the batch to the kernel. 64 KB keeps the send() amortised
  // without holding records long enough to add latency.
  static constexpr size_t kFlushAt = 64 << 10;

  bool all_fit(size_t len) const {
    for (const Conn& c : conns_)
      if (c.up && !c.tx.fits(len)) return false;
    return true;
  }

  size_t staged() const {
    size_t n = 0;
    for (const Conn& c : conns_) n += c.tx.size();
    return n;
  }
  int live() const {
    int n = 0;
    for (const Conn& c : conns_) n += c.up ? 1 : 0;
    return n;
  }
  void drop_conn(Conn& c, const char* why) {
    if (!c.up) return;
    c.up = false;
    ++closed_;
    if (c.eof)
      fprintf(stderr, "io_tcp: connection closed by the peer\n");
    else
      fprintf(stderr, "io_tcp: connection closed (%s: %s)\n", why,
              strerror(errno));
    close(c.fd);
    c.fd = -1;
  }

  bool server_ = false;
  int listen_fd_ = -1;
  uint32_t dgram_ = kDefaultDatagram;
  std::vector<Conn> conns_;
  uint64_t tx_records_ = 0, tx_bytes_ = 0, tx_refused_ = 0, tx_stalls_ = 0;
  uint64_t tx_partial_ = 0, rx_records_ = 0, rx_bytes_ = 0, syscalls_ = 0;
  uint64_t send_errs_ = 0, recv_errs_ = 0, desyncs_ = 0, closed_ = 0;
};

std::unique_ptr<Backend> TcpBackend::create(const Config& cfg,
                                            std::string* err) {
  auto b = std::unique_ptr<TcpBackend>(new TcpBackend());
  b->dgram_ = cfg.max_datagram;
  // cfg.listener is the *receiver* role. Over TCP the receiver is the client,
  // so the roles are the mirror image of the datagram backends.
  b->server_ = !cfg.listener;
  const uint64_t deadline = now_ms() + cfg.tcp_setup_ms;

  if (b->server_) {
    const int npeers = cfg.tcp_peers < 1 ? 1 : cfg.tcp_peers;
    b->listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (b->listen_fd_ < 0) {
      *err = std::string("socket: ") + strerror(errno);
      return nullptr;
    }
    const int one = 1;
    setsockopt(b->listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(cfg.port);
    if (bind(b->listen_fd_, reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) != 0) {
      *err = std::string("bind: ") + strerror(errno);
      return nullptr;
    }
    if (listen(b->listen_fd_, npeers + 4) != 0) {
      *err = std::string("listen: ") + strerror(errno);
      return nullptr;
    }
    // Block here until the receivers attach. The deadline (tcp_setup_ms)
    // exists so a misconfigured run fails instead of hanging forever.
    fprintf(stderr, "io_tcp: listening on :%u for %d peer(s)\n", cfg.port,
            npeers);
    while (static_cast<int>(b->conns_.size()) < npeers) {
      pollfd pf{b->listen_fd_, POLLIN, 0};
      const int pr = poll(&pf, 1, 200);
      if (pr < 0 && errno != EINTR) {
        *err = std::string("poll: ") + strerror(errno);
        return nullptr;
      }
      if (pr > 0) {
        sockaddr_in pa{};
        socklen_t plen = sizeof(pa);
        const int fd =
            accept(b->listen_fd_, reinterpret_cast<sockaddr*>(&pa), &plen);
        if (fd < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            continue;
          *err = std::string("accept: ") + strerror(errno);
          return nullptr;
        }
        tune(fd, cfg, true);
        b->conns_.emplace_back();
        Conn& c = b->conns_.back();
        c.fd = fd;
        c.up = true;
        c.peer.ip_be = pa.sin_addr.s_addr;
        c.peer.port_be = pa.sin_port;
        c.tx.init(kTxStage);
        c.rx.init(b->dgram_, kRxBuf);
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &pa.sin_addr, ip, sizeof(ip));
        fprintf(stderr, "io_tcp: peer %zu connected from %s:%u\n",
                b->conns_.size(), ip, ntohs(pa.sin_port));
      }
      if (now_ms() > deadline) {
        *err = "timed out waiting for " + std::to_string(npeers) +
               " peer(s) on port " + std::to_string(cfg.port);
        return nullptr;
      }
    }
    close(b->listen_fd_);  // no late joiners: the peer set is fixed at startup
    b->listen_fd_ = -1;
    return b;
  }

  // Client role (receiver). Retry until the server's listen() is up: the
  // receiver may well start first, so the first few connects are expected
  // to be refused.
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(cfg.port);
  if (inet_pton(AF_INET, cfg.dst_ip.c_str(), &sa.sin_addr) != 1) {
    *err = "bad server address " + cfg.dst_ip;
    return nullptr;
  }
  for (;;) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      *err = std::string("socket: ") + strerror(errno);
      return nullptr;
    }
    if (connect_until(fd, sa, deadline)) {
      tune(fd, cfg, false);
      b->conns_.emplace_back();
      Conn& c = b->conns_.back();
      c.fd = fd;
      c.up = true;
      c.peer.ip_be = sa.sin_addr.s_addr;
      c.peer.port_be = sa.sin_port;
      c.tx.init(kTxStage);
      c.rx.init(b->dgram_, kRxBuf);
      fprintf(stderr, "io_tcp: connected to %s:%u\n", cfg.dst_ip.c_str(),
              cfg.port);
      return b;
    }
    close(fd);
    if (now_ms() > deadline) {
      *err = "connect " + cfg.dst_ip + ":" + std::to_string(cfg.port) + ": " +
             strerror(errno);
      return nullptr;
    }
    const timespec ts{0, 2 * 1000 * 1000};  // 2 ms between attempts
    nanosleep(&ts, nullptr);
  }
}

}  // namespace

std::unique_ptr<Backend> make_tcp_backend(const Config& cfg, std::string* err) {
  return TcpBackend::create(cfg, err);
}

}  // namespace dgram_io

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// Open-loop round-trip load over any backend: the driver offers datagrams at
// a fixed rate whether or not replies have come back, `echo --role server` on
// the far host reflects them, and every reflection is timed against the send
// timestamp it carries. Unlike echo's one-in-flight ping-pong this measures
// latency *under load* -- queueing, batching and the stack's per-packet cost
// all show up, which is the regime a transport actually runs in.
//
//   far host:  ./echo --role server --io KIND [--dst DRIVER_IP for tcp*] ...
//   here:      ./loadgen --io KIND --dst FAR_IP --rate 800000 --secs 5
//
// Only one clock is read (CLOCK_MONOTONIC, on this host), so no cross-host
// synchronisation is needed; the price is that a number here is a round trip,
// two stack traversals each way. Output is one CSV line, header with --header.
//
// Pacing: each loop turn queues the datagrams whose due time has passed (at
// most kBurst), flushes once and reads replies, so when the loop falls behind
// it batches instead of stalling -- the same "ring drained, send now" policy a
// real sender uses. A case where the backend cannot keep up therefore shows as
// achieved rate < offered rate and a rising tail, not as a silent slowdown.
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "dgram_io/backend.h"

namespace {

uint64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

constexpr uint64_t kBurst = 64;  // datagrams queued per loop turn at most

struct Hdr {
  uint64_t seq;
  uint64_t t_send;
};

void usage() {
  fprintf(stderr,
          "usage: loadgen --io KIND --dst IP [options]\n"
          "  --rate N         offered datagrams/s (default 200000)\n"
          "  --secs S         measured duration (default 5)\n"
          "  --warmup S       unrecorded lead-in (default 0.5)\n"
          "  --size N         payload bytes, >= 16 (default 100)\n"
          "  --port N         (default 5000)\n"
          "  --label TEXT     first CSV column (default = --io)\n"
          "  --header         print the CSV header first\n"
          "  --ifname/--queue/--dst-mac/--xdp-copy  xdp, tcp-xdp\n"
          "  --dpdk-pci/--dpdk-ip/--dst-mac         dpdk, tcp-dpdk\n"
          "  --sqpoll/--sqpoll-cpu N                uring\n");
}

double pct(const std::vector<uint32_t>& v, double p) {
  if (v.empty()) return 0;
  const size_t i = std::min(v.size() - 1, static_cast<size_t>(p * v.size()));
  return v[i] / 1000.0;
}

}  // namespace

int main(int argc, char** argv) {
  dgram_io::Config cfg;
  cfg.port = 5000;
  double rate = 200000, secs = 5, warmup = 0.5;
  uint32_t size = 100;
  std::string label;
  bool header = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--io") cfg.kind = next();
    else if (a == "--dst") cfg.dst_ip = next();
    else if (a == "--rate") rate = atof(next().c_str());
    else if (a == "--secs") secs = atof(next().c_str());
    else if (a == "--warmup") warmup = atof(next().c_str());
    else if (a == "--size") size = static_cast<uint32_t>(atoi(next().c_str()));
    else if (a == "--port") cfg.port = static_cast<uint16_t>(atoi(next().c_str()));
    else if (a == "--label") label = next();
    else if (a == "--header") header = true;
    else if (a == "--ifname") cfg.ifname = next();
    else if (a == "--queue") cfg.queue = atoi(next().c_str());
    else if (a == "--dst-mac") cfg.dst_mac = next();
    else if (a == "--xdp-copy") cfg.force_copy = true;
    else if (a == "--dpdk-pci") cfg.dpdk_pci = next();
    else if (a == "--dpdk-ip") cfg.dpdk_ip = next();
    else if (a == "--sqpoll") cfg.uring_sqpoll = true;
    else if (a == "--sqpoll-cpu") cfg.uring_sqpoll_cpu = atoi(next().c_str());
    else { usage(); return 2; }
  }
  if (cfg.dst_ip.empty() || rate <= 0 || secs <= 0 || size < sizeof(Hdr) ||
      size > cfg.max_datagram) {
    usage();
    return 2;
  }
  if (label.empty()) label = cfg.kind;
  cfg.listener = false;  // the driving side; over TCP that makes us the server

  std::string err;
  auto net = dgram_io::make_backend(cfg, &err);
  if (!net) { fprintf(stderr, "backend: %s\n", err.c_str()); return 1; }

  const uint64_t n_warm = static_cast<uint64_t>(rate * warmup);
  const uint64_t n_total = n_warm + static_cast<uint64_t>(rate * secs);
  const double ns_per = 1e9 / rate;
  std::vector<uint32_t> rtt;
  rtt.reserve(n_total - n_warm);
  std::vector<uint8_t> payload(size, 0x5a);
  std::vector<dgram_io::RxPacket> rx(64);

  uint64_t sent = 0, got = 0, got_measured = 0, bad = 0, flushes = 0;
  uint64_t last_seq = 0, reordered = 0, refused = 0;
  const uint64_t t0 = now_ns();
  uint64_t t_last_send = t0;

  auto drain = [&]() {
    const int n = net->rx(rx.data(), static_cast<int>(rx.size()));
    if (n <= 0) return;
    const uint64_t t = now_ns();
    for (int k = 0; k < n; ++k) {
      if (!rx[k].data || rx[k].len < sizeof(Hdr)) { ++bad; continue; }
      Hdr h;
      std::memcpy(&h, rx[k].data, sizeof(h));
      if (h.seq >= n_total || h.t_send > t) { ++bad; continue; }
      if (got && h.seq < last_seq) ++reordered;
      last_seq = h.seq;
      ++got;
      if (h.seq >= n_warm) {
        ++got_measured;
        const uint64_t d = t - h.t_send;
        rtt.push_back(d > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(d));
      }
    }
  };

  while (sent < n_total) {
    const uint64_t t = now_ns();
    uint64_t due = static_cast<uint64_t>(static_cast<double>(t - t0) / ns_per) + 1;
    if (due > n_total) due = n_total;
    // At most kBurst per turn, then replies are read. A driver that has fallen
    // behind would otherwise spend whole milliseconds sending its backlog while
    // its own receive buffer overflows -- measuring its loop, not the path.
    if (due > sent + kBurst) due = sent + kBurst;
    if (sent < due) {
      while (sent < due) {
        Hdr h{sent, now_ns()};
        std::memcpy(payload.data(), &h, sizeof(h));
        // false = the backend's TX ring is full: back-pressure, not failure.
        // Push what is queued, service RX, and try the same datagram again.
        if (!net->queue(payload.data(), size)) {
          ++refused;
          net->flush();
          drain();
          continue;
        }
        ++sent;
      }
      net->flush();
      ++flushes;
      t_last_send = now_ns();
    }
    drain();
  }
  // Stragglers: wait out a generous bound for the last replies.
  while (got < sent && now_ns() - t_last_send < 500000000ull) drain();

  const double elapsed = static_cast<double>(t_last_send - t0) / 1e9;
  std::sort(rtt.begin(), rtt.end());
  const uint64_t measured = n_total - n_warm;
  if (header)
    printf("label,io,offered_mps,achieved_mps,size,sent,measured,lost,"
           "lost_pct,reordered,bad,tx_refused,msgs_per_flush,p50_us,p90_us,p99_us,"
           "p999_us,p9999_us,max_us\n");
  printf("%s,%s,%.0f,%.0f,%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64
         ",%.4f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
         label.c_str(), cfg.kind.c_str(), rate,
         elapsed > 0 ? static_cast<double>(sent) / elapsed : 0.0, size, sent,
         measured, measured - got_measured,
         100.0 * static_cast<double>(measured - got_measured) /
             static_cast<double>(measured),
         reordered, bad, refused,
         flushes ? static_cast<double>(sent) / static_cast<double>(flushes) : 0.0,
         pct(rtt, 0.50), pct(rtt, 0.90), pct(rtt, 0.99), pct(rtt, 0.999),
         pct(rtt, 0.9999), rtt.empty() ? 0.0 : rtt.back() / 1000.0);
  fflush(stdout);
  net->log_stats(stderr);
  return 0;
}

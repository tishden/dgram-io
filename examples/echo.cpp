// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// Ping-pong over any backend, to show what the interface actually buys: the
// two loops below never learn whether their datagrams are riding kernel UDP
// sockets (plain or through io_uring), an AF_XDP ring, a DPDK poll-mode
// driver, or a TCP stream with lwIP underneath it. Only the --io flag changes.
//
//   server:  ./echo --role server --io udp --port 5000
//   client:  ./echo --role client --io udp --port 5000 --dst 127.0.0.1 -n 10000
//
// The client prints the round-trip distribution measured with dgram_io/rtt.h.
// For the L2 backends both ends additionally need --ifname/--dst-mac (xdp) or
// --dpdk-pci/--dpdk-ip (dpdk); run `--help` for the whole list.
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dgram_io/backend.h"
#include "dgram_io/latbd.h"
#include "dgram_io/rtt.h"

namespace {

uint64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

void usage() {
  fprintf(stderr,
          "usage: echo --role server|client [options]\n"
          "  --io KIND        udp|uring|xdp|dpdk|tcp|tcp-dpdk|tcp-xdp (default udp)\n"
          "  --port N         UDP/TCP port (default 5000)\n"
          "  --dst IP         client: where to send\n"
          "  -n N             client: datagrams to send (default 10000)\n"
          "  --size N         payload bytes (default 64)\n"
          "  --ifname NAME    xdp: NIC to bind the socket to\n"
          "  --queue N        xdp: NIC queue index\n"
          "  --dst-mac MAC    xdp/dpdk: peer MAC\n"
          "  --xdp-copy       xdp: skip the zero-copy attempt\n"
          "  --dpdk-pci ADDR  dpdk: PCI address to take over\n"
          "  --dpdk-ip IP     dpdk: our IPv4\n"
          "  --sqpoll         uring: kernel thread polls the submission queue\n"
          "  --sqpoll-cpu N   uring: pin that thread to CPU N\n");
}

}  // namespace

int main(int argc, char** argv) {
  dgram_io::Config cfg;
  std::string role;
  uint64_t count = 10000;
  uint32_t size = 64;
  cfg.port = 5000;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--role") role = next();
    else if (a == "--io") cfg.kind = next();
    else if (a == "--port") cfg.port = static_cast<uint16_t>(atoi(next().c_str()));
    else if (a == "--dst") cfg.dst_ip = next();
    else if (a == "-n") count = strtoull(next().c_str(), nullptr, 10);
    else if (a == "--size") size = static_cast<uint32_t>(atoi(next().c_str()));
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
  if (role != "server" && role != "client") { usage(); return 2; }
  cfg.listener = (role == "server");

  // Line-buffered even into a file, so a server's log is readable while it runs.
  setvbuf(stdout, nullptr, _IOLBF, 0);

  std::string err;
  auto net = dgram_io::make_backend(cfg, &err);
  if (!net) { fprintf(stderr, "backend: %s\n", err.c_str()); return 1; }
  printf("%s on %s\n", role.c_str(), net->name());

  std::vector<dgram_io::RxPacket> rx(64);

  if (role == "server") {
    // Reflect whatever arrives, back to whoever sent it. queue_to() takes the
    // endpoint straight off the received packet, which is what lets one
    // server answer many clients without tracking any of them.
    uint64_t seen = 0;
    for (;;) {
      const int n = net->rx(rx.data(), static_cast<int>(rx.size()));
      for (int i = 0; i < n; ++i) {
        net->queue_to(rx[i].data, rx[i].len, rx[i].from);
        ++seen;
      }
      if (n > 0) net->flush();
      if (seen && seen % 100000 == 0) printf("reflected %" PRIu64 "\n", seen);
    }
  }

  // Client: one datagram in flight at a time, so the measurement is a clean
  // round trip rather than a throughput number wearing a latency costume.
  std::vector<uint8_t> payload(size, 0xa5);
  dgram_io::rtt::Estimator est;
  dgram_io::latbd::Stage stage("rtt");
  uint64_t got = 0;

  for (uint64_t i = 0; i < count; ++i) {
    std::memcpy(payload.data(), &i, sizeof(i) <= size ? sizeof(i) : size);
    const uint64_t t0 = now_ns();
    if (!net->queue(payload.data(), size)) { fprintf(stderr, "queue failed\n"); return 1; }
    net->flush();

    // Wait for the reflection, with a bound so a lost datagram does not wedge
    // the loop -- this example has no recovery layer, which is the point:
    // dgram_io carries packets, it does not promise to deliver them.
    const uint64_t deadline = t0 + 100000000ull;  // 100 ms
    for (;;) {
      const int n = net->rx(rx.data(), static_cast<int>(rx.size()));
      if (n > 0) {
        const uint64_t dt = now_ns() - t0;
        est.add_sample(dt);
        stage.add(dt);
        ++got;
        break;
      }
      if (now_ns() > deadline) break;
    }
  }

  printf("sent %" PRIu64 ", reflected %" PRIu64 " (%.2f%% lost)\n", count, got,
         count ? 100.0 * static_cast<double>(count - got) / static_cast<double>(count) : 0.0);
  if (est.have()) printf("rtt floor: %" PRIu64 " ns\n", est.min_ns());
  stage.report(stdout, "client");
  net->log_stats(stdout);
  return 0;
}

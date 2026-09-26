# Changelog

## 0.1.0 — unreleased

First public version.

* **Backends:** `udp` (sendmmsg/recvmmsg, unicast and multicast), `uring`
  (the same socket through io_uring: multishot receive into a provided-buffer
  ring, optional SQPOLL), `xdp` (AF_XDP, zero-copy with copy-mode fallback),
  `dpdk` (PMD behind vfio-pci), `tcp` (kernel), and `tcp-dpdk` / `tcp-xdp`
  (lwIP in-process over either bypass path). Every one is optional at build
  time; a compiled-out backend links and says what is missing.
* **Pieces:** Ethernet/IPv4/UDP frame build and parse (`pkt.h`), record
  framing for streams (`stream.h`), the AF_XDP socket wrapper
  (`xdp_socket.h`), DPDK port setup (`dpdk_port.h`), a per-stage latency
  breakdown (`latbd.h`) and a rolling-minimum RTT estimator (`rtt.h`).
* **Measuring:** `loadgen` (open-loop round-trip load), `bench_matrix.sh`
  (backends x rates over SSH or across a network namespace), `wire_bench.sh`
  (the whole matrix on two cabled ports of one host), `bench_report.py`,
  `uring_probe`. Results for an AWS c6in pair and a back-to-back 82599 are
  in [docs/BENCHMARKS.md](docs/BENCHMARKS.md).
* **Build:** `make install` with a pkg-config file (`dgram-io`).

Known issues, written up in docs/BENCHMARKS.md:

* `tcp-xdp` in AF_XDP zero-copy mode (seen on ixgbe) aborts its connection
  under load when lwIP's pbuf pool runs out; copy mode is not affected.
* `uring` with SQPOLL cannot pin its SQ thread on RHEL 9's 5.14 kernel,
  which refuses `IORING_SETUP_SQ_AFF`.

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

Found while benchmarking, fixed before the first release:

* AF_XDP zero-copy on an 82599 dropped every frame over 1 KB: ixgbe sizes its
  RX buffer from the UMEM chunk in whole kilobytes, and 2048-byte chunks gave
  1024. UMEM frames are 4096 bytes now, and both XDP backends refuse a
  `max_datagram` that does not fit one RX buffer instead of losing it on the
  wire. This is what broke `tcp-xdp` under load in zero-copy mode.
* `udp` returned a truncated datagram as an empty slot inside `rx()`'s count.
* `uring` with SQPOLL could crash when its submission queue filled.

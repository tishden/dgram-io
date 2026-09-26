# Changelog

## 0.1.0 — 2026-09-26

First public version.

* **Backends:** `udp` (sendmmsg/recvmmsg, unicast and multicast), `uring`
  (the same socket through io_uring: multishot receive into a provided-buffer
  ring, optional SQPOLL), `xdp` (AF_XDP, zero-copy with copy-mode fallback),
  `dpdk` (PMD behind vfio-pci), `tcp` (kernel), and `tcp-dpdk` / `tcp-xdp`
  (lwIP in-process over either bypass path). Every one is optional at build
  time; a compiled-out backend links and says what is missing.
* **Pieces:** Ethernet/IPv4/UDP frame build and parse (`pkt.h`), record
  framing for streams (`stream.h`), a per-stage latency breakdown
  (`latbd.h`) and a rolling-minimum RTT estimator (`rtt.h`). The AF_XDP
  socket wrapper, DPDK port setup and lwIP glue are internal (`src/`).
* **Measuring:** `loadgen` (open-loop round-trip load), `bench_matrix.sh`
  (backends x rates over SSH or across a network namespace), `wire_bench.sh`
  (the whole matrix on two cabled ports of one host), `bench_report.py`,
  `uring_probe`. Results for an AWS c6in pair and a back-to-back 82599 are
  in [docs/BENCHMARKS.md](docs/BENCHMARKS.md).
* **Build:** `make install` with a pkg-config file (`dgram-io`). The XDP
  backends find their filter objects where `make install` put them. lwIP's
  symbols in the static library are prefixed, so it links next to an lwIP of
  your own.

Found while benchmarking, fixed before the first release:

* AF_XDP zero-copy on an 82599 dropped every frame over 1 KB: ixgbe sizes its
  RX buffer from the UMEM chunk in whole kilobytes, and 2048-byte chunks gave
  1024. UMEM frames are 4096 bytes now, and both XDP backends refuse a
  `max_datagram` that does not fit one RX buffer instead of losing it on the
  wire. This is what broke `tcp-xdp` under load in zero-copy mode.
* `udp` returned a truncated datagram as an empty slot inside `rx()`'s count.
* `uring` with SQPOLL could crash when its submission queue filled.

Found in review before publishing, fixed in the same release:

* Stream backends with several peers could deliver a record twice: `queue()`
  refused it after some peers had already taken it. A record now goes to all
  live peers or to none.
* An empty datagram on a stream backend was taken for a framing error and
  closed the connection. Empty records are carried like any other.
* A stream whose connections were all gone looked idle (`rx()` returned 0);
  it now returns -1. Records that arrived before the peer's FIN are still
  delivered first.
* `dpdk` did not range-check `max_datagram`; every backend now refuses a size
  outside 1..8900 at setup.
* `uring` blocked inside `queue()` when every TX slot was in flight; it now
  returns false (back-pressure) like the other backends.
* `udp`: one failed datagram in `sendmmsg` dropped the rest of its batch, and
  an ICMP error from an earlier send made `rx()` report a failed datapath.
* Clearing a stale XDP program could detach other programs sharing the
  interface through a libxdp dispatcher.
* `rtt::Estimator` overflowed its index after 2^31 samples.
* The lwIP port reseeded the application's `random()`.
* `make CXXFLAGS=...` broke the build; the library is now built with -fPIC;
  `make uninstall` removes only the files it installed.
* `Config::queue` is `Config::xdp_queue`.

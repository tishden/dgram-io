# dgram-io

One datagram interface, seven datapaths. Write the packet loop once; choose at
runtime whether it rides kernel UDP sockets (through plain syscalls or
io_uring), an AF_XDP ring, a DPDK poll-mode driver, kernel TCP, or a userspace
TCP stack over either of the bypass paths.

```cpp
#include <dgram_io/backend.h>

dgram_io::Config cfg;
cfg.kind = "dpdk";           // or udp | uring | xdp | tcp | tcp-dpdk | tcp-xdp
cfg.dst_ip = "10.0.0.2";
cfg.port = 5000;

std::string err;
auto net = dgram_io::make_backend(cfg, &err);   // nullptr + err on failure

net->queue(payload, len);    // copies; your buffer is free immediately
net->flush();                // batch leaves here

dgram_io::RxPacket rx[64];
int n = net->rx(rx, 64);     // non-blocking; views valid until the next rx()
```

That is the whole interface. `queue` / `queue_to` / `flush` / `rx` /
`log_stats`, and a `Config` struct.

## Why bother

Because the interesting question is usually not "is DPDK faster than sockets"
— it is *how much faster, on this hardware, for this traffic, and is that
worth what it costs to operate*. You cannot answer that by reading; you have
to run the same workload across the paths. That is hard if switching paths
means rewriting the send loop, and easy if it means changing a string.

Here is the same transport, same wire, same rates, 200k messages per case,
36 cases, zero drops in every one — p50 microseconds, producer timestamp to
consumer:

| rate | dpdk | tcp-dpdk | xdp | tcp-xdp | udp | tcp |
|---|---|---|---|---|---|---|
| 200k msg/s | **4.19** | 4.98 | 7.17 | 13.32 | 9.75 | 20.08 |
| 800k msg/s | **4.20** | 5.14 | 10.48 | 15.92 | 12.56 | 27.22 |
| 1.6M msg/s | **4.58** | 5.97 | 12.74 | 26.29 | 14.54 | 27.72 |

Two things fall out of that table which are hard to see any other way:

**The cost of TCP is not a property of TCP.** The *same* lwIP stack costs
+0.8 us over a DPDK PMD, +6.2 us over AF_XDP, and the kernel's own TCP costs
+10.3 us over kernel UDP. Protocol cost and kernel cost are different
quantities, and the second is an order of magnitude larger. A per-stage
breakdown confirms it directly: `ring_wait`, `build` and `publish` agree
across five of the six datapaths to within 0.1 us — the entire difference
lives in the "stack + wire" segment.

**Nagle costs more than the choice of stack.** With `TCP_NODELAY` off, the
same `tcp-dpdk` path gives 40.0 us p50 instead of 4.98 and 78.8 us p99 instead
of 9.8 — worse than kernel TCP. On a stream of hundred-byte messages every
write is small by definition, so the algorithm holds back essentially
everything. `Config::tcp_nodelay` defaults to true here for that reason.

### Every backend, two stands

That table came from the transport this library was extracted from. The
numbers below were measured with this repository's own tools
([docs/BENCHMARKS.md](docs/BENCHMARKS.md), raw data in
[`bench/results/`](bench/results)): round trips of 100-byte messages, one
per packet, an open-loop driver offering 20k to 1.6M msg/s, each side
busy-polling on one isolated core. *82599* is one desktop whose two Intel
82599 10G ports are cabled to each other; *AWS* is two c6in.4xlarge in a
cluster placement group. "Holds up to" is the highest offered rate at which
that rate and every lower one were delivered in full, with the p50 there.

| backend | 82599: p50 at 20k msg/s | 82599: holds up to | AWS: p50 at 20k msg/s | AWS: holds up to |
|---|---|---|---|---|
| `dpdk` | **7.4 us** | 1.6M (7.9 us) | **17.4 us** | 200k (32.2 us) |
| `tcp-dpdk` | 8.3 us | 1.6M (10.2 us) | 18.4 us | 1.6M (2.5 ms) |
| `xdp`, zero-copy | 12.8 us | 1.6M (16.7 us) | — | — |
| `xdp`, copy mode | 11.4 us | 1.6M (25.9 us) | 29.8 us | 200k (41.4 us) |
| `tcp-xdp`, zero-copy | 13.9 us | 1.6M (18.6 us) | — | — |
| `tcp-xdp`, copy mode | 12.8 us | 1.6M (19.9 us) | 31.7 us | 400k (89.9 us) ¹ |
| `udp` | 17.2 us | 400k (37.0 us) | 21.0 us | 200k (37.4 us) |
| `tcp` | 18.9 us | **1.6M (28.7 us)** | 21.4 us | **1.6M (52.8 us)** |
| `uring` | 20.1 us | 400k (66.1 us) | 38.4 us | 200k (82.9 us) ² |
| `uring` + SQPOLL | 20.5 us | 200k (29.0 us) | 34.1 us | 200k (54.2 us) |

What it says:

* **On real hardware the bypass paths are flat.** DPDK moves 7.4 to 7.9 us
  across an 80x range of load; kernel UDP starts at 17 us and falls over past
  400k.
* **On AWS no datagram path gets past about 380k round trips/s**, DPDK
  included -- the same DPDK loop that does 1.6M on a 2012 desktop CPU. That
  points at the ENA path (one queue, one flow) rather than the loop; no AWS
  allowance counter moved.
* **Streams carry the rate by packing** several messages into a segment.
  Kernel TCP does it without queueing: on AWS it is the only path under 60 us
  p50 at 1.6M, where lwIP keeps the rate but queues into milliseconds.
* **io_uring is not a faster `udp`**: 2.9 us slower at low load on the 82599,
  17 us on AWS (see [The backends](#the-backends)); SQPOLL buys little and
  costs capacity.
* **Past its ceiling a bypass path queues, a kernel socket drops**: on AWS
  DPDK and AF_XDP sit at 6-19 ms with under 0.3% lost, kernel UDP loses half.

¹ The main run's 800k case fell to 513k msg/s; two other runs held 800k at
about 1 ms. ² 53.9 us in the repeat.

Running this found two bugs that are fixed here: AF_XDP zero-copy on the
82599 dropped every frame over 1 KB (the RX buffer it derives from a
2048-byte UMEM chunk), and the benchmark script's own CPU confinement --
both in [docs/BENCHMARKS.md](docs/BENCHMARKS.md#found-by-these-runs).

## The backends

| `kind` | what it is | needs |
|---|---|---|
| `udp` | kernel UDP sockets, `sendmmsg`/`recvmmsg`, unicast + multicast | nothing |
| `uring` | the same UDP socket through io_uring: multishot recv into a provided-buffer ring, so an idle `rx()` is no syscall; optional `SQPOLL` | liburing, kernel 6.0+ (or backport), `kernel.io_uring_disabled=0` |
| `xdp` | AF_XDP socket on one NIC queue, kernel stack bypassed | libxdp, root |
| `dpdk` | the NIC entirely in user space behind `vfio-pci` | DPDK, root, hugepages |
| `tcp` | kernel TCP; sender is the server, receiver the client | nothing |
| `tcp-dpdk` | lwIP in-process, frames over the DPDK PMD | DPDK + lwIP |
| `tcp-xdp` | lwIP in-process, frames over an AF_XDP socket | libxdp + lwIP |

`uring` is not a faster `udp`. Measured under load it trails plain
`sendmmsg`/`recvmmsg` by 2.9 us p50 on an 82599 and by 17 us on AWS
([docs/BENCHMARKS.md](docs/BENCHMARKS.md)): a receive goes through the
interrupt, softirq and task_work before its completion is visible, and does
not get the socket's busy polling. What it does buy is an idle `rx()` that
costs no syscall and a `flush()` that costs one per batch (or none under
`SQPOLL`). One trap, refused at setup: with `SQPOLL` and a caller pinned to a
single CPU, the kernel's SQ thread inherits that CPU and starves behind the
busy-polling caller, so set `uring_sqpoll_cpu` to another core.

Everything is optional and detected at build time. `make config` prints what
this machine has; a build with none of the optional dependencies still gives
you `udp` and `tcp`. A backend that was compiled out still links — calling it
returns a clear error naming what is missing, instead of failing to build.

## Contract, and why it is shaped this way

* **`queue()` copies.** Your buffer is reusable the instant it returns.
  Datagrams accumulate up to an internal batch and leave on `flush()` or when
  the batch fills. Deciding *when* to flush — the "ring drained, send now"
  policy — belongs to the caller, not here, because only the caller knows
  whether more work is coming.
* **`rx()` is non-blocking** and returns at most `max` datagrams. The returned
  views stay valid until the next `rx()` call. On the stream backends that is
  what forces the deframer to move its leftover tail only at the *start* of
  the next round, after you are done with the previous batch.
* **`Endpoint` carries a MAC**, because the L2 backends address whole frames.
  The UDP backend leaves it zeroed and ignores it on transmit.
* **A backend that cannot carry the requested `max_datagram` fails at setup.**
  Loudly, on purpose: truncating on receive looks exactly like packet loss,
  gets repaired by whatever recovery layer sits above, and hides a
  misconfiguration behind a plausible number.

## Also in here

Pieces that were worth separating out of the backends:

* **`pkt.h`** — Ethernet/IPv4/UDP frame construction and parsing as pure
  functions over caller-owned buffers. No sockets, no libxdp, so it is
  unit-testable without root (`tests/test_pktbuild.cpp`). Deliberately narrow:
  IPv4 only, no fragmentation, UDP checksum zero (legal, and nothing on these
  paths verifies it), IP header checksum computed properly so a kernel-UDP
  peer still accepts our frames.
* **`stream.h`** — record framing for the TCP backends. A `[u16 len][record]`
  prefix and a deframer that reassembles across arbitrary chunk boundaries.
  Two bytes per datagram, 0.14% at the 1400-byte default.
* **`xdp_socket.h`** — the AF_XDP/XSK setup that is otherwise a day of reading
  kernel headers: UMEM, the four rings, the zero-copy attempt with a copy-mode
  fallback, and frame lifetime managed so the `rx()` contract above holds.
* **`latbd.h` / `rtt.h`** — per-stage latency breakdown, and an RTT estimator
  that keeps a rolling *minimum*. Minimum rather than mean because sparse
  polling adds a one-sided error to every sample: the floor of the
  distribution is the path RTT, the mean is not.

## Build and try it

```
make config     # what this machine can build
make            # the library plus any XDP filter objects
make test       # unit tests: framing, deframing, RTT. No NIC, no root.
make example
sudo make install   # optional: /usr/local, or PREFIX=... / DESTDIR=...
```

Installed, it is `pkg-config --cflags --libs dgram-io`. The library is
static, so those flags carry every backend's dependencies that this build
compiled in. The XDP filters go to `pkg-config --variable=bpfdir dgram-io`;
point `Config::bpf_obj` there, since the default looks next to the running
binary.

The example is a ping-pong that runs over any backend:

```
./bin/echo --role server --io udp --port 5000
./bin/echo --role client --io udp --port 5000 --dst 127.0.0.1 -n 10000
```

```
client on udp
sent 5000, reflected 5000 (0.00% lost)
rtt floor: 8147 ns
client: latbd stage=rtt n=5000 p50_us=8.78 p99_us=12.38 p999_us=16.92 max_us=67.06
io_udp: syscalls=5000 send_errs=0 rx_truncated=0
```

(that is loopback on a noisy desktop — it is a smoke test, not a measurement)

For `tcp-dpdk` / `tcp-xdp`, fetch the TCP stack first:

```
scripts/get_lwip.sh && make
```

## Measuring

`bin/echo` is one datagram in flight. For latency *under load* there is an
open-loop driver and the scripts that produced
[docs/BENCHMARKS.md](docs/BENCHMARKS.md):

* **`bin/loadgen`** offers datagrams at a fixed rate to an `echo --role
  server` and prints one CSV line: achieved rate, loss, reordering, p50 to
  max round trip.
* **`scripts/bench_matrix.sh`** runs backends x rates between two hosts over
  SSH, or on one host with the reflector in a network namespace.
* **`scripts/wire_bench.sh`** does the whole matrix, as root, on one machine
  whose two NIC ports are cabled together: namespace, NIC tuning, runtime CPU
  isolation, the DPDK bind, and everything put back on exit.
* **`scripts/bench_report.py`** turns a results directory into the tables.
* **`bin/uring_probe`** says which io_uring setups the running kernel accepts.

## What this is not

It is not a transport. There is no reliability, no ordering, no congestion
control, no fan-out policy — `dgram_io` moves packets and tells you what
happened. Loss recovery, if you want it, layers on top; the codec this was
built alongside is at [rsfec](https://github.com/tishden/rs-fec).

It is Linux and x86-64. The kernel-socket backends would port anywhere; AF_XDP
and DPDK would not.

## License and contributing

Apache-2.0, Copyright 2026 Denis Tishkov. The two XDP programs
(`src/*.bpf.c`) are dual-licensed Apache-2.0 OR GPL-2.0-only, because the
kernel loads a BPF program only when it declares a GPL-compatible licence.
Contributions are accepted under the Developer Certificate of Origin — no
CLA, just a `Signed-off-by` line. See [CONTRIBUTING.md](CONTRIBUTING.md).
Changes are listed in [CHANGELOG.md](CHANGELOG.md).

Written with Claude (Anthropic). The design decisions, the measurements and
the hardware runs are mine; the code was written in collaboration with it.

Third-party dependencies are not vendored here and keep their own licences;
see [THIRD-PARTY.md](THIRD-PARTY.md).

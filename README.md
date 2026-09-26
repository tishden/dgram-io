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

`uring` is not a faster `udp` by default. On an 82599 (ixgbe, two ports on
one host joined by an AOC cable, `scripts/wire_peer.sh`), a 64-byte ping-pong
with one datagram in flight gives p50 RTT 28.7 us for `udp` against 31.9 us
for `uring` and 30.3 us with `SQPOLL`: a single receive goes through poll
wakeup and task_work before its completion is visible, one hop more than a
`recvmmsg` that finds the datagram already queued. What it does buy is an idle
`rx()` that costs no syscall and a `flush()` that costs one per batch (or none
under `SQPOLL`). Whether that wins is a question for your traffic, which is
the point of having both behind one string. One trap, refused at setup: with
`SQPOLL` and a caller pinned to a single CPU, the kernel's SQ thread inherits
that CPU and starves behind the busy-polling caller, so set
`uring_sqpoll_cpu` to another core.

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
```

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

## What this is not

It is not a transport. There is no reliability, no ordering, no congestion
control, no fan-out policy — `dgram_io` moves packets and tells you what
happened. Loss recovery, if you want it, layers on top; the codec this was
built alongside is at [rsfec](https://github.com/tishden/rs-fec).

It is Linux and x86-64. The kernel-socket backends would port anywhere; AF_XDP
and DPDK would not.

## License and contributing

Apache-2.0, Copyright 2026 Denis Tishkov. Contributions are accepted under the
Developer Certificate of Origin — no CLA, just a `Signed-off-by` line. See
[CONTRIBUTING.md](CONTRIBUTING.md).

Written with Claude (Anthropic). The design decisions, the measurements and
the hardware runs are mine; the code was written in collaboration with it.

Third-party dependencies are not vendored here and keep their own licences;
see [THIRD-PARTY.md](THIRD-PARTY.md).

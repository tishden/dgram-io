# Benchmarks

Round-trip latency under load for every backend, on two very different
stands: a pair of AWS instances, and one desktop whose two 10G ports are
cabled to each other. Measured 2026-09-26. Raw CSVs, logs and a passport of
each machine are in [`bench/results/`](../bench/results).

The short version:

* **On real hardware the bypass paths are flat.** Back to back on an 82599,
  `dpdk` holds 7.4-7.9 us p50 from 20k to 1.6M messages/s, and `tcp-dpdk`
  8.3-10.2 us. Kernel `udp` starts at 17 us and falls over past 400k.
* **On AWS everything is slower, and datagrams hit a ceiling.** The same
  `dpdk` starts at 17.4 us, and no datagram backend sustains more than about
  380k round trips/s -- including DPDK, which does 1.6M/s on the desktop.
* **TCP carries the rate by packing.** A stream puts several messages in one
  segment, so the stream backends reach 1.6M/s on both stands where one
  message per datagram cannot.
* **io_uring does not win here.** `uring` is 3 us slower than `udp` at low
  load on the 82599 and 17 us slower on AWS.

## Method

`bin/loadgen` offers datagrams at a fixed rate, whether or not replies have
come back; `bin/echo --role server` on the other side reflects each one with
the same backend; the driver times every reflection against the send
timestamp it carries. One clock on one host, so no cross-host sync is
involved -- and every figure is a **round trip**: two stack traversals, two
wire crossings.

* 100-byte messages, one per datagram (one framed record on the streams).
* Offered 20k, 100k, 200k, 400k, 800k and 1.6M messages/s; 5 s measured per
  case after an unrecorded warm-up (0.5 s on AWS, 1 s locally: ixgbe retrains
  its link whenever XDP attaches). 100k to 8M round trips per case.
* The driver queues at most 64 datagrams per loop turn, flushes, then reads
  replies. When it falls behind it batches instead of stalling, so a backend
  that cannot keep up shows as *achieved < offered* and a queue, not as a
  quiet slowdown.
* Lost = no reflection 0.5 s after the last send.
* Driver and reflector each busy-poll on one isolated physical core.
* A subset (20k and 200k for every backend, plus the high rates of the
  unstable cells) was run twice; below saturation the repeats agree within
  0.1-2.5 us p50 unless noted.

In the tables a cell is **p50 / p99 in microseconds**. A case that did not
keep up -- achieved under 99% of offered, or over 0.1% lost -- shows as
`sat. <achieved>, <loss>` instead: past that point the latency is the depth
of a queue. On a stream, loss means the connection failed, shown as
`broken`. `scripts/bench_report.py <dir>` prints these tables from the CSVs.

## The stands

| | AWS | local |
| --- | --- | --- |
| machines | 2 x c6in.4xlarge (virtualized), cluster placement group, ap-northeast-1a | 1 x Core i7-3820 (Sandy Bridge-E, 4 cores, 2012), both ends on one host |
| CPU | Xeon Platinum 8375C 2.9 GHz, 8 cores x 2 | i7-3820 3.6 GHz, 4 cores x 2 |
| NIC and link | ENA, second ENI, through the VPC | Intel 82599ES (ixgbe), port 0 to port 1 over an AOC cable, the peer port in its own network namespace |
| kernel | 7.0.0-1013-aws (Ubuntu 24.04) | 5.14.0-611.49.2.el9_7 (AlmaLinux 9.7) |
| isolation | `isolcpus`, `nohz_full`, `rcu_nocbs` on cores 2-5 | no reboot: every systemd slice confined to CPUs 0,1,4,5 at runtime, workqueues too; driver on core 2, reflector on core 3 |
| NIC tuning | 1 queue, rx/tx-usecs 0, IRQ on CPU 0 | 1 queue per port, rx-usecs 0, IRQs on CPU 0 and 1 (`irq-house`), irqbalance off |
| AF_XDP | native, copy mode only (ena offered no zero-copy) | native, zero-copy; copy mode measured separately |
| DPDK | 23.11, ena PMD, vfio-pci no-IOMMU, write-combining BAR | 25.11, ixgbe PMD, vfio-pci no-IOMMU |
| sysctls | busy_poll 50, busy_read 50, rmem/wmem max 64 MB | the same |
| built with | liburing 2.5, libxdp 1.4.2, lwIP 2.2.1 | liburing 2.5, libxdp 1.6.2, lwIP 2.2.1 |
| tooling | [aws-lowlat-stand](https://github.com/tishden/aws-lowlat-stand) + `scripts/bench_matrix.sh` | `scripts/wire_bench.sh` |

## Results: 82599 back to back (`bench/results/2026-09-26-ixgbe-irq-house`)

| backend | 20k | 100k | 200k | 400k | 800k | 1.6M |
| --- | --- | --- | --- | --- | --- | --- |
| dpdk | 7.4 / 9.2 | 7.4 / 8.0 | 7.4 / 7.8 | 7.5 / 7.9 | 7.7 / 8.1 | 7.9 / 8.8 |
| tcp-dpdk | 8.4 / 10.1 | 8.3 / 10.1 | 8.4 / 9.8 | 8.5 / 10.7 | 8.6 / 13.0 | 10.2 / 18.7 |
| xdp | 12.9 / 40.3 | 12.0 / 38.0 | 12.2 / 29.4 | 12.3 / 38.5 | 13.3 / 38.6 | 15.0 / 39.0 |
| xdp-copy | 11.4 / 13.4 | 11.4 / 18.3 | 11.4 / 21.8 | 12.4 / 17.1 | 15.0 / 21.4 | 25.7 / 81.1 |
| tcp-xdp | 14.1 / 40.7 | broken, 100% lost | broken, 100% lost | broken, 100% lost | broken, 100% lost | broken, 100% lost |
| tcp-xdp-copy | 13.1 / 33.1 | 12.6 / 38.4 | 12.4 / 34.7 | 14.6 / 41.5 | 17.2 / 47.2 | 19.1 / 48.6 |
| udp | 17.3 / 23.8 | 17.0 / 30.4 | 18.9 / 33.6 | 37.4 / 72.0 | sat. 600k, 50.0% | sat. 612k, 49.9% |
| tcp | 19.0 / 26.2 | 19.0 / 41.7 | 23.9 / 52.4 | 24.3 / 59.0 | 26.1 / 60.3 | 31.1 / 71.0 |
| uring | 20.7 / 41.8 | 20.1 / 45.5 | 24.2 / 53.0 | 65.2 / 133.1 | sat. 452k, 0.0% | sat. 453k, 0.0% |

`uring` with SQPOLL has no row: this kernel refused it (see *Open issues*).
`tcp-xdp` in zero-copy mode breaks from 100k; the same stack in copy mode is
fine, so this is a bug, not a result (see *Open issues*).

## Results: AWS c6in.4xlarge pair (`bench/results/2026-09-26-aws-c6in.4xlarge`)

| backend | 20k | 100k | 200k | 400k | 800k | 1.6M |
| --- | --- | --- | --- | --- | --- | --- |
| dpdk | 17.4 / 22.8 | 18.8 / 30.0 | 32.2 / 44.1 | sat. 377k, 0.0% | sat. 377k, 0.1% | sat. 379k, 0.0% |
| tcp-dpdk | 18.4 / 24.3 | 18.9 / 27.4 | 38.3 / 53.5 | 78.0 / 115.8 | 2035.2 / 2229.3 | 2545.2 / 2723.2 |
| xdp (copy mode) | 29.8 / 38.3 | 29.2 / 41.6 | 41.4 / 56.7 | sat. 371k, 0.3% | sat. 388k, 0.1% | sat. 386k, 0.1% |
| tcp-xdp (copy mode) | 31.7 / 52.6 | 31.9 / 54.4 | 47.1 / 69.9 | 89.9 / 124.4 | sat. 513k, 0.0% | 1226.2 / 1348.9 |
| udp | 21.0 / 25.1 | 22.6 / 29.7 | 37.4 / 50.0 | sat. 400k, 11.9% | sat. 562k, 59.9% | sat. 561k, 60.0% |
| tcp | 21.4 / 26.7 | 23.3 / 30.7 | 40.8 / 56.4 | 42.5 / 56.1 | 42.0 / 56.1 | 52.8 / 71.2 |
| uring | 38.4 / 42.9 | 38.2 / 54.9 | 82.9 / 117.9 | sat. 400k, 8.2% | sat. 569k, 70.6% | sat. 568k, 70.7% |
| uring-sqpoll | 34.1 / 38.8 | 34.6 / 49.2 | 54.2 / 74.6 | sat. 400k, 9.3% | sat. 566k, 68.0% | sat. 577k, 70.8% |

Two cells did not repeat: `uring` at 200k gave 53.9 us p50 the second time,
and `tcp-xdp` at 800k held the rate at 0.98 ms p50 in the repeat (and 1.09 ms
in an earlier pass). No AWS allowance counter (`pps_allowance_exceeded`,
`bw_in/out_allowance_exceeded`) moved on either host during the session.

## What the numbers say

**The AWS datagram ceiling is not in dgram-io's loop.** On AWS five very
different datagram paths -- kernel sockets, io_uring, AF_XDP, DPDK -- all top
out at 350-410k packets/s per direction, and the stream backends reach
1.6M messages/s only in about 400k frames/s (tcp-dpdk: 8.8M records in 2.24M
frames). The same DPDK loop on a 2012 desktop CPU does 1.6M round trips/s at
7.9 us. What is left on AWS is the virtual NIC path: one ENA queue pair per
host (the stand configures one, so the XSK sees all traffic), one flow, and
an LLQ descriptor write per packet. Which of those binds was not separated;
a multi-queue run would.

**Past the ceiling, bypass paths queue and kernel sockets drop.** On AWS
DPDK settles at a 6-7 ms queue with no loss (`tx_drop=0`, `hw_imissed=0`) and
AF_XDP at 17-19 ms with under 0.3% lost; kernel UDP turns a 64 MB socket
buffer into a 20-150 ms queue and then loses half.

**Streams pack, and the kernel's TCP packs best.** From 200k to 1.6M/s
kernel `tcp` stays at 24-31 us p50 locally and 41-53 us on AWS, carrying up
to 9.5 messages per flush locally at the top. lwIP packs only once it is
behind: on AWS `tcp-dpdk` and `tcp-xdp` keep the rate but queue into
milliseconds from 800k. Locally `tcp-dpdk` never gets there.

**AF_XDP copy mode beat zero-copy on the 82599 below 800k** -- 11.4 against
12.0-12.9 us p50, and a much tighter p99 (13-22 against 29-40 us). Zero-copy
takes over from 800k, where copy mode's per-packet copy starts to cost
(15.0 against 13.3 us, and 25.7 against 15.0 at 1.6M). On ENA there was no
choice: copy mode only.

**io_uring is the slowest kernel path at every rate.** `uring` trails `udp` by
3.4 us p50 at 20k on the 82599 and by 17 us on AWS; locally it holds 400k at
65 us p50 where `udp` does 37, and caps at 452k. `net.core.busy_read` lets
`recvmmsg` spin on the NIC queue; a multishot receive only sees a packet after
the interrupt, softirq and task_work have run, and io_uring polls NAPI only
when registered for it (`IORING_REGISTER_NAPI`, not used yet). That is the
likely gap, not verified. Past saturation on AWS `uring` also reordered
datagrams (0.2-2.5 million per case) -- once the send buffer is full, pending
SENDMSGs are retried from poll wake-ups without an order between them.

**Where the IRQ goes matters as much as a backend choice.** The same local
matrix with each port's IRQ on the idle hyperthread of its process's core
([`2026-09-26-ixgbe-irq-sibling`](../bench/results/2026-09-26-ixgbe-irq-sibling))
instead of a housekeeping CPU:

| backend | 20k p50, IRQ on housekeeping CPU | 20k p50, IRQ on HT sibling | highest sustained rate, housekeeping / sibling |
| --- | --- | --- | --- |
| udp | 17.3 us | 20.1 us | 400k / 200k |
| uring | 20.7 us | 25.5 us | 400k / 200k |
| xdp (zero-copy) | 12.9 us | 15.7 us | 1.6M / 1.6M |
| tcp | 19.0 us | 21.9 us | 1.6M / 1.6M |
| dpdk | 7.4 us | 7.4 us | 1.6M / 1.6M |

The softirq on the sibling hyperthread competes with the busy-polling
process for the core. DPDK has no interrupt and does not care. On a busy
desktop the housekeeping CPU is shared with everything else, though: an
earlier pass with that layout lost 50-62% in the kernel paths at 200k while
the machine was in use (that pass's raw data was overwritten by the next
one; the figures are from its console output).

## Open issues found by these runs

* **tcp-xdp breaks in zero-copy mode (ixgbe).** From 100k/s -- and at 20k in
  one repeat -- the reflector's lwIP runs out of pool pbufs (`pbuf_err` 39 to
  452 per case), then aborts the connection (`tcp_proterr=1`, `closed=2-3`);
  the driver sees `ERR_ABRT` and refuses every write after that. Copy mode on
  the same ports runs clean to 1.6M. `PBUF_POOL_SIZE` is 2048 against a 1 MB
  `TCP_WND`; why only zero-copy exhausts it is not established.
* **uring with SQPOLL on RHEL 9.7's 5.14.** `bin/uring_probe` shows the kernel
  accepts SQPOLL but refuses `IORING_SETUP_SQ_AFF` with EINVAL. The backend
  then falls back to creating the ring from the target CPU so the SQ thread
  inherits it; in the benchmark that fallback also failed. The backend now
  reports which step failed; the next `wire_bench.sh` run records the answer
  (and the scope's cpuset) in its passport. 7.0-aws accepted SQ_AFF.
* **SQPOLL with the caller pinned to one CPU** starves the SQ thread, which
  inherits that CPU: round trips went from 30 us to milliseconds on the
  82599. The backend refuses that configuration at setup.

## Caveats

* Every figure is a round trip. Halving it approximates one way only
  loosely: the two directions carry different load.
* The local stand is one host. Both ends share memory, L3 and the PCIe root;
  two machines would add nothing to the wire but remove that sharing.
* The local stand has no boot-time isolation (`isolcpus`, `nohz_full`), only
  runtime cpusets; the kernel's own threads can still land on the benchmark
  cores. It is a desktop, not a lab machine.
* Kernel backends get a second core for free -- their softirq runs where the
  IRQ is -- while DPDK and lwIP do everything on one.
* One instance type, one AZ, 100-byte messages, MTU 1500, one message per
  packet. Larger messages or application-level batching move every ceiling.
* These are not comparable to the one-way table in the README, which came
  from a different transport with its own batching.

## Reproduce

Locally, with two ports of one NIC cabled together (as root; restores the
host on exit, about 20 minutes):

```
scripts/get_lwip.sh && make all example
sudo scripts/wire_bench.sh                     # IRQ_LAYOUT=sibling for the A/B
scripts/bench_report.py bench/results/<date>-ixgbe-irq-house
```

On AWS, with [aws-lowlat-stand](https://github.com/tishden/aws-lowlat-stand)
(`instance_type = "c6in.4xlarge"`, `receiver_count = 1`, `use_spot = false`):

```
make aws-up KEY=<key> ANSIBLE_VARS='-e workload_src=<dgram-io> -e workload_dst=dgram-io -e "workload_build=scripts/get_lwip.sh && make -j16 all example"'
export SND=ubuntu@<sender> RCV=ubuntu@<receiver> SSH_KEY=<key.pem>
scripts/bench_matrix.sh "udp uring uring-sqpoll tcp xdp tcp-xdp" "20000 100000 200000 400000 800000 1600000" > kernel.csv
make aws-dpdk-bind
scripts/bench_matrix.sh "dpdk tcp-dpdk" "20000 100000 200000 400000 800000 1600000" > dpdk.csv
make aws-down
```

The AWS stand for these numbers ran 13:02-14:25 UTC and was destroyed
afterwards; its passport is written from what was read off the hosts during
the session.

# Benchmarks

Round-trip latency under load for every backend, on two very different
stands: a pair of AWS instances, and one desktop whose two 10G ports are
cabled to each other. Measured 2026-09-26. Raw CSVs, logs and a passport of
each machine are in [`bench/results/`](bench/results).

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
* **Two silent failures were found by running this**, and fixed: AF_XDP
  zero-copy on the 82599 dropped every frame over 1 KB, and a benchmark
  script left its CPU confinement behind (see *Found by these runs*).

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
| dpdk | 7.4 / 8.1 | 7.4 / 7.8 | 7.4 / 7.8 | 7.5 / 8.0 | 7.7 / 8.7 | 7.9 / 9.9 |
| tcp-dpdk | 8.3 / 9.6 | 8.3 / 9.8 | 8.4 / 10.4 | 8.5 / 11.9 | 8.6 / 14.4 | 10.2 / 20.5 |
| xdp | 12.8 / 15.9 | 12.1 / 25.3 | 12.1 / 17.1 | 14.4 / 19.1 | 14.7 / 20.9 | 16.7 / 23.4 |
| xdp-copy | 11.4 / 13.2 | 11.3 / 18.1 | 11.4 / 21.9 | 12.5 / 17.2 | 14.9 / 21.4 | 25.9 / 77.0 |
| tcp-xdp | 13.9 / 16.8 | 13.2 / 26.8 | 14.7 / 25.4 | 15.1 / 20.9 | 16.9 / 23.6 | 18.6 / 27.3 |
| tcp-xdp-copy | 12.8 / 15.1 | 13.8 / 19.9 | 13.2 / 25.2 | 15.5 / 22.4 | 17.9 / 25.3 | 19.9 / 29.1 |
| udp | 17.2 / 18.9 | 17.0 / 21.3 | 19.6 / 29.3 | 37.0 / 59.5 | sat. 613k, 50.0% | sat. 621k, 50.0% |
| tcp | 18.9 / 23.4 | 19.0 / 25.9 | 24.0 / 36.9 | 23.4 / 37.6 | 25.0 / 38.4 | 28.7 / 46.7 |
| uring | 20.1 / 25.2 | 19.5 / 31.5 | 24.3 / 39.1 | 66.1 / 152.0 | sat. 430k, 0.0% | sat. 427k, 0.0% |
| uring-sqpoll | 20.5 / 24.5 | 20.6 / 28.1 | 29.0 / 46.6 | sat. 400k, 76.1% | sat. 594k, 85.1% | sat. 591k, 85.2% |

Everything here held its rate without loss up to 1.6M except the kernel
socket paths: `udp` and `uring` saturate past 400k, and `uring` with SQPOLL
already at 400k -- where it loses 76% while plain `uring` still delivers
everything.

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
kernel `tcp` stays at 23-29 us p50 locally and 41-53 us on AWS, carrying up
to 9.4 messages per flush locally at the top. lwIP packs only once it is
behind: on AWS `tcp-dpdk` and `tcp-xdp` keep the rate but queue into
milliseconds from 800k. Locally `tcp-dpdk` never gets there.

**AF_XDP copy mode beat zero-copy on the 82599 up to 400k** -- 11.3-12.5
against 12.1-14.4 us p50, with a similar p99. The two meet at 800k (14.9 and
14.7 us), and at 1.6M copy mode's per-packet copy costs it: 25.9 against
16.7 us. The same holds for the lwIP stack on top (`tcp-xdp` 18.6 us at 1.6M
in zero-copy, 19.9 in copy mode). On ENA there was no choice: copy mode
only.

**io_uring is the slowest kernel path at every rate.** `uring` trails `udp` by
2.9 us p50 at 20k on the 82599 and by 17 us on AWS; locally it holds 400k at
66 us p50 where `udp` does 37, and caps at 430k. SQPOLL buys nothing at low
load locally (20.5 against 20.1 us) and 4 us on AWS, and costs capacity. `net.core.busy_read` lets
`recvmmsg` spin on the NIC queue; a multishot receive only sees a packet after
the interrupt, softirq and task_work have run, and io_uring polls NAPI only
when registered for it (`IORING_REGISTER_NAPI`, not used yet). That is the
likely gap, not verified. Past saturation on AWS `uring` also reordered
datagrams (0.2-2.5 million per case) -- once the send buffer is full, pending
SENDMSGs are retried from poll wake-ups without an order between them.

**Where the IRQ goes matters as much as a backend choice.** The same local
matrix with each port's IRQ on the idle hyperthread of its process's core
([`2026-09-26-ixgbe-irq-sibling`](bench/results/2026-09-26-ixgbe-irq-sibling),
an earlier build: its `tcp-xdp` row shows the zero-copy bug described
below, and it has no `uring-sqpoll`) instead of a housekeeping CPU:

| backend | 20k p50, IRQ on housekeeping CPU | 20k p50, IRQ on HT sibling | highest sustained rate, housekeeping / sibling |
| --- | --- | --- | --- |
| udp | 17.2 us | 20.1 us | 400k / 200k |
| uring | 20.1 us | 25.5 us | 400k / 200k |
| xdp (zero-copy) | 12.8 us | 15.7 us | 1.6M / 1.6M |
| tcp | 18.9 us | 21.9 us | 1.6M / 1.6M |
| dpdk | 7.4 us | 7.4 us | 1.6M / 1.6M |

The softirq on the sibling hyperthread competes with the busy-polling
process for the core. DPDK has no interrupt and does not care. On a busy
desktop the housekeeping CPU is shared with everything else, though: an
earlier pass with that layout lost 50-62% in the kernel paths at 200k while
the machine was in use (that pass's raw data was overwritten by the next
one; the figures are from its console output).

## Found by these runs

* **AF_XDP zero-copy on the 82599 dropped every frame over 1 KB.** In
  zero-copy mode ixgbe sizes the 82599's RX buffer from the UMEM chunk minus
  the kernel's 256-byte headroom, in whole kilobytes rounded down. With the
  usual 2048-byte chunks that is 1024 bytes; a longer frame needs a second
  descriptor, which the zero-copy path does not handle, and the frame is
  dropped where no XSK counter sees it. 100-byte datagrams never cross the
  line. `tcp-xdp` did as soon as lwIP coalesced records into full segments:
  one such segment was lost with every retransmission, the connection
  stalled behind it and lwIP aborted it -- found with the lwIP trace
  (`DGRAM_IO_LWIP_TRACE=1`). The datagram `xdp` backend would have lost
  everything at its default 1400-byte `max_datagram`. UMEM frames are now
  4096 bytes (3072 after the rounding), both XDP backends refuse a
  `max_datagram` that does not fit one RX buffer, and 1200-byte datagrams run
  clean in zero-copy (`xdp` 17.1 us, `tcp-xdp` 19.3 us p50 at 200k).
* **The local benchmark left the host confined to 4 of its 8 CPUs.** An
  empty `AllowedCPUs=` drops systemd's property but not the cgroup's cpuset,
  so the restore step undid nothing; and the benchmark's own scope was sized
  with `nproc`, which inside the confined slice counts 4. The second mistake
  is what made `uring` with SQPOLL fail: its SQ threads' CPUs were outside
  the scope, and the kernel rightly refused to pin them there. Both fixed;
  `bin/uring_probe` (in the passport) now records what the scope may use.
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
sudo DRV_IF=enp3s0f0 PEER_IF=enp3s0f1 scripts/wire_bench.sh   # IRQ_LAYOUT=sibling for the A/B
scripts/bench_report.py bench/results/<date>-ixgbe-irq-house
```

On AWS, with [aws-lowlat-stand](https://github.com/tishden/aws-lowlat-stand)
(`instance_type = "c6in.4xlarge"`, `receiver_count = 1`, `use_spot = false`):

```
make aws-up KEY=<key> SSH_CIDR=<your-ip>/32 TFVARS='-var instance_type=c6in.4xlarge -var receiver_count=1 -var use_spot=false' ANSIBLE_VARS='-e workload_src=<dgram-io> -e workload_dst=dgram-io -e "workload_build=scripts/get_lwip.sh && make -j16 all example"'
export SND=ubuntu@<sender> RCV=ubuntu@<receiver> SSH_KEY=<key.pem>
scripts/bench_matrix.sh "udp uring uring-sqpoll tcp xdp tcp-xdp" "20000 100000 200000 400000 800000 1600000" > kernel.csv
make aws-dpdk-bind
scripts/bench_matrix.sh "dpdk tcp-dpdk" "20000 100000 200000 400000 800000 1600000" > dpdk.csv
make aws-down
```

The AWS stand for these numbers ran 13:02-14:25 UTC and was destroyed
afterwards; its passport is written from what was read off the hosts during
the session.

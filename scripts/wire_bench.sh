#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The full round-trip matrix (scripts/bench_matrix.sh) on ONE host whose two
# NIC ports are cabled to each other: the driver on the first port, the
# reflector on the second inside its own network namespace, so every
# datagram really crosses the cable. Same backends, rates and repeat subset as
# the AWS run in docs/BENCHMARKS.md.
#
#   scripts/get_lwip.sh && make all example     # as yourself, first
#   sudo scripts/wire_bench.sh [OUTDIR]         # ~30 min, restores everything
#
# What it changes for the run, and puts back on any exit (a trap, so ^C too):
#  * both ports: one combined queue, interrupt moderation off (rx-usecs 0);
#    the peer port moves into netns $NS (scripts/wire_peer.sh);
#  * CPUs: no reboot, so no isolcpus -- instead every systemd slice is
#    confined to $HOUSE_CPUS at runtime and the kernel workqueues with it,
#    irqbalance is stopped, and each port's IRQ is pinned to its own
#    housekeeping CPU. The benchmark runs in its own slice with all CPUs;
#    driver and reflector get one physical core each, their HT siblings are
#    left idle except as SQPOLL threads;
#  * sysctls as aws-lowlat-stand sets them (busy_poll/busy_read 50, rmem/wmem
#    max 64 MB) and io_uring enabled;
#  * for the DPDK half both ports are bound to vfio-pci (no-IOMMU mode when
#    the host has no IOMMU) and back to their driver afterwards.
#
# Results land in OUTDIR (default bench/results/<date>-<nic>): the CSVs, the
# matrix logs and a passport of the machine as it was measured.
set -euo pipefail
export LC_ALL=C   # the passport greps tool output in English

[ "$(id -u)" = 0 ] || { echo "run as root: sudo $0 $*" >&2; exit 1; }

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DRV_IF=${DRV_IF:-enp3s0f0}
PEER_IF=${PEER_IF:-enp3s0f1}
PEER_ADDR=${PEER_ADDR:-192.168.10.101/24}
NS=${NS:-dgpeer}
DRV_CPU=${DRV_CPU:-2} DRV_SQ=${DRV_SQ:-6}   # physical core 2 and its sibling
RFL_CPU=${RFL_CPU:-3} RFL_SQ=${RFL_SQ:-7}   # physical core 3 and its sibling
HOUSE_CPUS=${HOUSE_CPUS:-0,1,4,5}
HOUSE_MASK=${HOUSE_MASK:-33}                # the same set as a hex cpumask
DRV_IRQ_CPU=${DRV_IRQ_CPU:-0} PEER_IRQ_CPU=${PEER_IRQ_CPU:-1}
KINDS_KERNEL=${KINDS_KERNEL:-"udp uring uring-sqpoll tcp xdp tcp-xdp"}
KINDS_DPDK=${KINDS_DPDK:-"dpdk tcp-dpdk"}
RATES=${RATES:-"20000 100000 200000 400000 800000 1600000"}
REPEAT=${REPEAT:-1}
OUT=${1:-$ROOT/bench/results/$(date +%F)-$(ethtool -i "$DRV_IF" | awk '/^driver:/{print $2}')}
RUN_AS=${SUDO_USER:-root}

for f in bin/echo bin/loadgen bin/xdp_filter.bpf.o bin/xdp_tcp_filter.bpf.o; do
  [ -e "$ROOT/$f" ] || { echo "$f missing: build first (scripts/get_lwip.sh && make all example)" >&2; exit 1; }
done
cfg=$(make -s -C "$ROOT" config)
for b in uring xdp dpdk lwip; do
  grep -q "^$b: *yes" <<<"$cfg" || { echo "built without $b:"; echo "$cfg"; exit 1; } >&2
done
[ -e "/run/dgram-io-wire-peer.$NS" ] && { echo "netns $NS is up already: sudo scripts/wire_peer.sh down" >&2; exit 1; }

mkdir -p "$OUT"
log() { echo "wire_bench: $*" | tee -a "$OUT/run.log" >&2; }

pci_of() { ethtool -i "$1" | awk '/^bus-info:/{print $2}'; }
mac_of() { cat "/sys/class/net/$1/address"; }
irqs_of() { awk -F: -v i="$1" '$0 ~ i {gsub(/ /,"",$1); print $1}' /proc/interrupts; }
chan_of() { ethtool -l "$1" | awk '/^Current/{c=1} c && /^Combined/{print $2; exit}'; }
usecs_of() { ethtool -c "$1" | awk '/^rx-usecs:/{print $2}'; }

DRV_PCI=$(pci_of "$DRV_IF") PEER_PCI=$(pci_of "$PEER_IF")
DRV_MAC=$(mac_of "$DRV_IF") PEER_MAC=$(mac_of "$PEER_IF")
DRV_DRIVER=$(ethtool -i "$DRV_IF" | awk '/^driver:/{print $2}')
DRV_ADDRS=$(ip -4 -o addr show dev "$DRV_IF" | awk '{print $4}' | xargs)
DRV_IP=${DRV_ADDRS%%/*}
PEER_IP=${PEER_ADDR%%/*}
[ -n "$DRV_IP" ] || { echo "$DRV_IF has no IPv4 address" >&2; exit 1; }

# ------------------------------------------------------------ save + restore
declare -A OLD
for k in net.core.busy_poll net.core.busy_read net.core.rmem_max net.core.wmem_max; do
  OLD[$k]=$(sysctl -n "$k")
done
OLD[drv_chan]=$(chan_of "$DRV_IF") OLD[peer_chan]=$(chan_of "$PEER_IF")
OLD[drv_usecs]=$(usecs_of "$DRV_IF") OLD[peer_usecs]=$(usecs_of "$PEER_IF")
OLD[irqbalance]=$(systemctl is-active irqbalance 2>/dev/null || true)
OLD[wq]=$(cat /sys/devices/virtual/workqueue/cpumask)
OLD[noiommu]=$(cat /sys/module/vfio/parameters/enable_unsafe_noiommu_mode 2>/dev/null || echo N)

restore() {
  set +e
  trap - EXIT INT TERM
  log "restoring the host"
  pkill -KILL -f '[b]in/echo --role server|[b]in/loadgen --io'
  # Ports back to their kernel driver if the DPDK half left them bound.
  for p in "$DRV_PCI" "$PEER_PCI"; do
    [ "$(basename "$(readlink -f /sys/bus/pci/devices/$p/driver)")" = "$DRV_DRIVER" ] ||
      dpdk-devbind.py -b "$DRV_DRIVER" "$p" >/dev/null 2>&1
  done
  sleep 2
  ip link set "$DRV_IF" xdp off 2>/dev/null
  ip -n "$NS" link set "$PEER_IF" xdp off 2>/dev/null
  "$ROOT/scripts/wire_peer.sh" down >/dev/null 2>&1
  for a in $DRV_ADDRS; do ip addr add "$a" dev "$DRV_IF" 2>/dev/null; done
  ip link set "$DRV_IF" up; ip link set "$PEER_IF" up
  ethtool -L "$DRV_IF" combined "${OLD[drv_chan]}" 2>/dev/null
  ethtool -L "$PEER_IF" combined "${OLD[peer_chan]}" 2>/dev/null
  ethtool -C "$DRV_IF" rx-usecs "${OLD[drv_usecs]}" 2>/dev/null
  ethtool -C "$PEER_IF" rx-usecs "${OLD[peer_usecs]}" 2>/dev/null
  systemctl set-property --runtime system.slice AllowedCPUs= 2>/dev/null
  systemctl set-property --runtime user.slice AllowedCPUs= 2>/dev/null
  systemctl set-property --runtime init.scope AllowedCPUs= 2>/dev/null
  echo "${OLD[wq]}" > /sys/devices/virtual/workqueue/cpumask
  [ "${OLD[irqbalance]}" = active ] && systemctl start irqbalance
  for k in net.core.busy_poll net.core.busy_read net.core.rmem_max net.core.wmem_max; do
    sysctl -qw "$k=${OLD[$k]}"
  done
  [ -e /sys/module/vfio/parameters/enable_unsafe_noiommu_mode ] &&
    echo "${OLD[noiommu]}" > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode 2>/dev/null
  chown -R "$RUN_AS": "$OUT"
  log "restored: $DRV_IF ${DRV_ADDRS}, $PEER_IF back in the default namespace, CPUs/IRQs/sysctls as before"
}
trap restore EXIT INT TERM

# ------------------------------------------------------------ set up
log "output: $OUT"
sysctl -qw net.core.busy_poll=50 net.core.busy_read=50 \
  net.core.rmem_max=67108864 net.core.wmem_max=67108864
for i in "$DRV_IF" "$PEER_IF"; do
  ethtool -L "$i" combined 1
  ethtool -C "$i" rx-usecs 0
done
WIRE_SERVERS=0 PEER_IF=$PEER_IF PEER_ADDR=$PEER_ADDR NS=$NS "$ROOT/scripts/wire_peer.sh" up >>"$OUT/run.log" 2>&1

systemctl stop irqbalance 2>/dev/null || true
pin_irqs() {
  for q in $(irqs_of "$DRV_IF"); do echo "$DRV_IRQ_CPU" > "/proc/irq/$q/smp_affinity_list"; done
  for q in $(irqs_of "$PEER_IF"); do echo "$PEER_IRQ_CPU" > "/proc/irq/$q/smp_affinity_list"; done
}
pin_irqs
systemctl set-property --runtime system.slice AllowedCPUs="$HOUSE_CPUS"
systemctl set-property --runtime user.slice AllowedCPUs="$HOUSE_CPUS"
systemctl set-property --runtime init.scope AllowedCPUs="$HOUSE_CPUS"
echo "$HOUSE_MASK" > /sys/devices/virtual/workqueue/cpumask

passport() {
  {
    echo "date: $(date -u +%FT%TZ)"
    # root reading a user's checkout: git refuses without safe.directory.
    local g=(git -c safe.directory="$ROOT" -C "$ROOT")
    echo "dgram-io: $("${g[@]}" rev-parse --short HEAD)$("${g[@]}" diff --quiet HEAD || echo '-dirty')"
    echo "kernel: $(uname -r)"
    echo "cmdline: $(cat /proc/cmdline)"
    lscpu | grep -E "Model name|^CPU\(s\)|Thread|MHz"
    echo "governor: $(cat /sys/devices/system/cpu/cpu$DRV_CPU/cpufreq/scaling_governor 2>/dev/null)"
    echo "layout: driver cpu $DRV_CPU (sqpoll $DRV_SQ), reflector cpu $RFL_CPU (sqpoll $RFL_SQ), housekeeping $HOUSE_CPUS"
    echo "irqs: $DRV_IF -> cpu $DRV_IRQ_CPU, $PEER_IF -> cpu $PEER_IRQ_CPU, irqbalance stopped"
    echo "driver port: $DRV_IF $DRV_PCI $DRV_MAC $DRV_IP"
    echo "peer port:   $PEER_IF $PEER_PCI $PEER_MAC $PEER_IP (netns $NS)"
    ethtool -i "$DRV_IF" | head -3
    lspci -s "$DRV_PCI"
    echo "channels: $(chan_of "$DRV_IF"), rx-usecs: $(usecs_of "$DRV_IF")"
    sysctl net.core.busy_poll net.core.busy_read net.core.rmem_max kernel.io_uring_disabled
    grep -E "^(uring|xdp|dpdk|lwip):" <<<"$cfg"
    pkg-config --modversion liburing libxdp libdpdk | paste -sd' ' | sed 's/^/liburing libxdp dpdk: /'
  } > "$OUT/passport.txt" 2>&1
}
passport

matrix() {  # $1 = file stem, $2 = kinds, $3 = rates, $4 = port base
  systemd-run --quiet --scope --slice=dgbench.slice -p AllowedCPUs=0-$(($(nproc) - 1)) -- \
    env SUDO= SND=local RCV="netns:$NS" REMOTE_DIR="$ROOT" WARMUP=1 PORT_BASE="$4" \
      S_FACTS="IF=$DRV_IF IP=$DRV_IP MAC=$DRV_MAC PCI=$S_PCI DP0=$DRV_CPU DP1=$DRV_SQ" \
      R_FACTS="IF=$PEER_IF IP=$PEER_IP MAC=$PEER_MAC PCI=$R_PCI DP0=$RFL_CPU DP1=$RFL_SQ" \
      "$ROOT/scripts/bench_matrix.sh" "$2" "$3" > "$OUT/$1.csv" 2> "$OUT/$1.log"
  log "$1: $(($(wc -l < "$OUT/$1.csv") - 1)) cases"
}

# ------------------------------------------------------------ kernel netdev half
S_PCI= R_PCI=
log "kernel half: $KINDS_KERNEL x $RATES"
matrix kernel "$KINDS_KERNEL" "$RATES" 6000
if [ "$REPEAT" = 1 ]; then
  matrix kernel.repeat "$KINDS_KERNEL" "20000 200000" 6100
  matrix kernel.repeat2 "tcp-xdp tcp" "800000 1600000" 6200
fi

# ------------------------------------------------------------ DPDK half
log "binding $DRV_PCI and $PEER_PCI to vfio-pci"
modprobe vfio-pci
if [ -z "$(ls -A /sys/kernel/iommu_groups 2>/dev/null)" ]; then
  echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
  log "no IOMMU: vfio no-IOMMU mode"
fi
ip link set "$DRV_IF" down
ip -n "$NS" link set "$PEER_IF" down
# --force: the peer's netdev lives in another namespace, which devbind's
# "is this interface in use" check cannot see into anyway.
dpdk-devbind.py --force -b vfio-pci "$DRV_PCI" "$PEER_PCI"
S_PCI=$DRV_PCI R_PCI=$PEER_PCI
log "dpdk half: $KINDS_DPDK x $RATES"
matrix dpdk "$KINDS_DPDK" "$RATES" 6300
if [ "$REPEAT" = 1 ]; then
  matrix dpdk.repeat "$KINDS_DPDK" "20000 200000 800000" 6400
fi

log "done"

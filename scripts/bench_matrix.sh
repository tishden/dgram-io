#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Round-trip latency matrix across backends and offered rates. One CSV line
# per case on stdout.
#
#   SND=ubuntu@10.0.0.1 RCV=ubuntu@10.0.0.2 SSH_KEY=key.pem \
#   S_FACTS="IF=ens6 IP=10.0.1.1 MAC=.. PCI= DP0=2 DP1=3" \
#   R_FACTS="IF=ens6 IP=10.0.1.2 MAC=.. PCI= DP0=2 DP1=3" \
#     scripts/bench_matrix.sh "udp uring uring-sqpoll tcp xdp tcp-xdp" \
#                             "20000 200000 800000 1600000"
#
# SND and RCV are where each side runs: user@host (SSH), `local`, or
# `netns:NAME` (this host, inside that network namespace -- two cabled ports
# of one machine, see scripts/wire_bench.sh). S_FACTS / R_FACTS describe each
# host's data path: IF interface, IP and MAC on it, PCI address (for the DPDK
# kinds, once bound to vfio-pci), DP0 the core the process is pinned to, DP1
# a second core for the SQPOLL thread. Without them the facts are read from
# env files on the host (/etc/stand-data.env: DATA_IF DATA_IP DATA_MAC;
# /etc/stand-cores.env: DP0 DP1; optional /run/stand_dpdk.env: LOCAL_PCI
# LOCAL_MAC), which is how the AWS hosts in docs/BENCHMARKS.md were set up.
#
# The driver (bin/loadgen) runs on SND, the reflector (bin/echo --role server)
# on RCV, each pinned to DP0. The kernel-netdev backends and the DPDK ones
# cannot share a run: DPDK needs the data NIC bound to vfio-pci, which removes
# the netdev the others use. Bind between the two halves.
#
# Meant for dedicated benchmark hosts: before every case it kills any
# bin/echo server and bin/loadgen on both hosts and detaches whatever XDP
# program is on the data interface.
#
# Env: SECS (5), WARMUP (0.5), SIZE (100), REMOTE_DIR (dgram-io; absolute
#      for local targets),
#      PORT_BASE (6000; each case gets its own port, so no TIME_WAIT carry-over).
set -euo pipefail

KINDS=${1:?kinds}
RATES=${2:?rates}
: "${SND:?}" "${RCV:?}"
SECS=${SECS:-5}
WARMUP=${WARMUP:-0.5}
SIZE=${SIZE:-100}
DIR=${REMOTE_DIR:-dgram-io}
PORT=${PORT_BASE:-6000}
SUDO=${SUDO-sudo}   # empty when already root, or for the unprivileged kinds
SSH=(ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new
     -o ServerAliveInterval=15 ${SSH_KEY:+-i "$SSH_KEY" -o IdentitiesOnly=yes})

# sshd throttles bursts of new connections from one source, and a matrix opens
# several per case: 255 is ssh's own failure, so that one is retried.
on() {
  local h=$1 rc; shift
  case $h in
    local) bash -c "$*"; return ;;
    netns:*) ip netns exec "${h#netns:}" bash -c "$*"; return ;;
  esac
  for try in 1 2 3 4 5 6; do
    "${SSH[@]}" "$h" "$@" && return 0 || rc=$?
    [ "$rc" = 255 ] || return "$rc"
    sleep $((try * 2))
  done
  return "$rc"
}

# Host facts, once: data interface/IP/MAC, pinned cores, PCI (if bound).
facts() {
  on "$1" 'set -a; . /etc/stand-data.env; . /etc/stand-cores.env;
           [ -r /run/stand_dpdk.env ] && . /run/stand_dpdk.env;
           echo "IF=$DATA_IF IP=$DATA_IP MAC=${LOCAL_MAC:-$DATA_MAC} PCI=${LOCAL_PCI:-} DP0=$DP0 DP1=$DP1"'
}
declare -A S R
for kv in ${S_FACTS:-$(facts "$SND")}; do S[${kv%%=*}]=${kv#*=}; done
for kv in ${R_FACTS:-$(facts "$RCV")}; do R[${kv%%=*}]=${kv#*=}; done
echo "# sender   ${S[*]@K}" >&2
echo "# receiver ${R[*]@K}" >&2

# Per-kind arguments: $1 = side (S|R), echo them for that side's command.
args_for() {
  local kind=$1 side=$2
  local -n me=$side
  local peer; [ "$side" = S ] && peer=R || peer=S
  local -n them=$peer
  case $kind in
    udp|uring|tcp) echo "--io $kind" ;;
    uring-sqpoll) echo "--io uring --sqpoll --sqpoll-cpu ${me[DP1]}" ;;
    xdp) echo "--io xdp --ifname ${me[IF]} --dst-mac ${them[MAC]}" ;;
    tcp-xdp) echo "--io tcp-xdp --ifname ${me[IF]}" ;;
    # The same two with the zero-copy attempt skipped: where the NIC does
    # zero-copy (ixgbe, not ena) this is the copy-mode arm of the comparison.
    xdp-copy) echo "--io xdp --xdp-copy --ifname ${me[IF]} --dst-mac ${them[MAC]}" ;;
    tcp-xdp-copy) echo "--io tcp-xdp --xdp-copy --ifname ${me[IF]}" ;;
    dpdk) echo "--io dpdk --dpdk-pci ${me[PCI]} --dpdk-ip ${me[IP]} --dst-mac ${them[MAC]}" ;;
    tcp-dpdk) echo "--io tcp-dpdk --dpdk-pci ${me[PCI]} --dpdk-ip ${me[IP]}" ;;
    *) echo "unknown kind $kind" >&2; return 1 ;;
  esac
}

# [b]in: the pattern must not match the remote shell that carries it.
stop_reflector() {
  on "$RCV" "$SUDO pkill -INT -f '[b]in/echo --role server'; for i in \$(seq 50); do pgrep -f '[b]in/echo --role server' >/dev/null || exit 0; sleep 0.2; done; $SUDO pkill -KILL -f '[b]in/echo --role server'; exit 0"
}

# Both hosts back to a known state before every case: no leftover driver or
# reflector, and no XDP program on the data netdev. A process killed hard
# leaves its program attached, and an attached program changes the ena RX
# buffer mode under the *kernel* backends that run next -- a quiet skew, and
# the next XSK bind fails outright (E2BIG). Skipped when the netdev is gone
# (bound to vfio-pci for DPDK).
clean() {
  on "$1" "$SUDO pkill -KILL -f '[b]in/echo --role server|[b]in/loadgen --io'; sleep 0.3
           if ip link show $2 >/dev/null 2>&1; then
             $SUDO ip link set dev $2 xdp off 2>/dev/null
             ! ip link show $2 | grep -q 'prog/xdp' || { echo 'XDP program still attached on $2' >&2; exit 1; }
           fi; exit 0"
}

# The reflector's logs, in a directory only this run can know the name of
# (the reflector may run as root, so a fixed /tmp name would be an invitation).
RLOG=$(on "$RCV" 'mktemp -d /tmp/dgram-io-refl.XXXXXX')
trap 'on "$RCV" "rm -rf $RLOG" >/dev/null 2>&1 || true' EXIT

header=--header
for kind in $KINDS; do
  for rate in $RATES; do
    PORT=$((PORT + 1))
    sargs=$(args_for "$kind" S)
    rargs=$(args_for "$kind" R)
    clean "$SND" "${S[IF]}"
    clean "$RCV" "${R[IF]}"
    # Reflector: --dst matters only to the stream kinds, whose receiving side
    # is the TCP client and dials the driver.
    refl="cd $DIR; $SUDO nohup setsid timeout $((SECS * 4 + 120)) taskset -c ${R[DP0]} ./bin/echo --role server $rargs --port $PORT --dst ${S[IP]} > $RLOG/refl.$kind.$rate.log 2>&1 < /dev/null &"
    drv="cd $DIR && $SUDO taskset -c ${S[DP0]} ./bin/loadgen $sargs --dst ${R[IP]} --port $PORT --rate $rate --secs $SECS --warmup $WARMUP --size $SIZE --label $kind $header"
    case $kind in
      tcp*)
        # Driver is the TCP server: it must be listening before the reflector dials.
        out=$( { on "$SND" "$drv" 2> >(sed "s/^/# $kind $rate drv: /" >&2) & pid=$!;
                 sleep 3; on "$RCV" "$refl"; wait $pid; } ) || true ;;
      *)
        on "$RCV" "$refl"
        # DPDK EAL init and the XSK bind take a few seconds before the
        # reflector is polling; a UDP socket is ready at once.
        case $kind in dpdk) sleep 8 ;; xdp) sleep 3 ;; *) sleep 1 ;; esac
        out=$(on "$SND" "$drv" 2> >(sed "s/^/# $kind $rate drv: /" >&2)) || true ;;
    esac
    if [ -n "$out" ]; then
      echo "$out"
      header=
    else
      echo "# $kind $rate: no result" >&2
    fi
    # SIGINT makes the reflector print its own counters on the way out, so
    # the log has both ends of every case.
    stop_reflector
    on "$RCV" "cat $RLOG/refl.$kind.$rate.log" 2>/dev/null |
      grep -v "skipping unrecognized data section" |
      sed "s/^/# $kind $rate refl: /" >&2 || true
  done
done

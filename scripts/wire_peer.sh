#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Two ports of one NIC cabled to each other, on one host: the kernel would
# short-circuit traffic between two local addresses, so the peer port moves
# into its own network namespace and datagrams really cross the cable.
# Starts echo servers there (udp, uring, uring+sqpoll) for bin/echo clients
# in the default namespace.
#
#   make example
#   sudo scripts/wire_peer.sh up      # namespace + servers
#   ./bin/echo --role client --io uring --dst 192.168.10.101 --port 5000
#   sudo scripts/wire_peer.sh down    # everything back as it was
#
# Override with env: PEER_IF, PEER_ADDR, NS. Servers run as the invoking user,
# not root; their output goes to bin/wire_peer.<kind>.log. WIRE_SERVERS=0
# sets up the namespace only (scripts/wire_bench.sh starts its own).
set -euo pipefail

PEER_IF=${PEER_IF:-enp3s0f1}
PEER_ADDR=${PEER_ADDR:-192.168.10.101/24}
NS=${NS:-dgpeer}
STATE=/run/dgram-io-wire-peer.$NS
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ECHO=$ROOT/bin/echo
RUN_AS=${SUDO_USER:-root}

# port  extra-args          log name
SERVERS=(
  "5001 --io udp            udp"
  "5000 --io uring          uring"
  "5002 --io uring --sqpoll uring-sqpoll"
)

[ "$(id -u)" = 0 ] || { echo "run as root: sudo $0 $*" >&2; exit 1; }

up() {
  [ -x "$ECHO" ] || { echo "$ECHO missing: run 'make example' first" >&2; exit 1; }
  [ -e "$STATE" ] && { echo "already up ($STATE); run '$0 down' first" >&2; exit 1; }

  # Remember what we change, so down restores it exactly.
  {
    echo "IO_URING_DISABLED=$(sysctl -n kernel.io_uring_disabled 2>/dev/null || echo 0)"
    echo "OLD_ADDRS='$(ip -4 -o addr show dev "$PEER_IF" | awk '{print $4}' | xargs)'"
  } > "$STATE"

  sysctl -qw kernel.io_uring_disabled=0
  ip netns add "$NS"
  ip link set "$PEER_IF" netns "$NS"
  ip -n "$NS" addr flush dev "$PEER_IF"
  ip -n "$NS" addr add "$PEER_ADDR" dev "$PEER_IF"
  ip -n "$NS" link set lo up
  ip -n "$NS" link set "$PEER_IF" up

  [ "${WIRE_SERVERS:-1}" = 0 ] && SERVERS=()
  for s in "${SERVERS[@]}"; do
    read -r port rest <<< "$s"
    name=${rest##* }
    args=${rest% *}
    log=$ROOT/bin/wire_peer.$name.log
    # shellcheck disable=SC2086  # args is a word list on purpose
    ip netns exec "$NS" setpriv --reuid="$RUN_AS" --regid="$(id -g "$RUN_AS")" \
      --init-groups "$ECHO" --role server $args --port "$port" \
      > "$log" 2>&1 &
    echo "PID_$name=$!" >> "$STATE"
    echo "server $name on port $port (log $log)"
  done

  # The ixgbe link retrains after the move; wait for carrier before returning.
  for _ in $(seq 30); do
    [ "$(ip netns exec "$NS" cat /sys/class/net/"$PEER_IF"/carrier 2>/dev/null)" = 1 ] && break
    sleep 1
  done
  sleep 1
  for s in "${SERVERS[@]}"; do
    read -r _ rest <<< "$s"; name=${rest##* }
    head -1 "$ROOT/bin/wire_peer.$name.log"
  done
  echo "peer $PEER_ADDR on $PEER_IF in netns $NS, carrier=$(ip netns exec "$NS" cat /sys/class/net/"$PEER_IF"/carrier)"
}

down() {
  [ -e "$STATE" ] || { echo "not up (no $STATE)" >&2; exit 1; }
  # Parsed, not sourced: server names like uring-sqpoll are not shell names.
  IO_URING_DISABLED=$(sed -n 's/^IO_URING_DISABLED=//p' "$STATE")
  OLD_ADDRS=$(sed -n "s/^OLD_ADDRS='\(.*\)'$/\1/p" "$STATE")
  for pid in $(sed -n 's/^PID_[^=]*=//p' "$STATE"); do
    kill "$pid" 2>/dev/null || true
  done
  # Deleting the namespace hands the physical port back to the default one.
  ip netns del "$NS"
  for _ in $(seq 10); do ip link show "$PEER_IF" >/dev/null 2>&1 && break; sleep 0.5; done
  for a in $OLD_ADDRS; do ip addr add "$a" dev "$PEER_IF" 2>/dev/null || true; done
  ip link set "$PEER_IF" up
  sysctl -qw kernel.io_uring_disabled="$IO_URING_DISABLED"
  rm -f "$STATE"
  echo "restored $PEER_IF ($OLD_ADDRS), kernel.io_uring_disabled=$IO_URING_DISABLED"
}

case "${1:-}" in
  up) up ;;
  down) down ;;
  *) echo "usage: $0 up|down" >&2; exit 2 ;;
esac

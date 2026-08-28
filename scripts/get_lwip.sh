#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Fetch the TCP stack the tcp-dpdk and tcp-xdp backends run on.
#
# This project does not implement TCP. lwIP is a mature, widely deployed,
# BSD-licensed implementation (RFC 793 plus the congestion-control,
# fast-retransmit and window-scaling work that followed). What lives here is
# the ~500 lines that let it drive a DPDK port or an AF_XDP socket instead of a
# kernel driver, and the framing that turns its byte stream back into
# datagrams.
#
# Pinned to a release tag on purpose: a measurement names a stack version, and
# "whatever master was that day" is not a version.
#
# Usage: scripts/get_lwip.sh [DIR]      (default third_party/lwip)
set -euo pipefail

TAG=${TAG:-STABLE-2_2_1_RELEASE}
DIR=${1:-$(dirname "$0")/../third_party/lwip}
REPO=${REPO:-https://github.com/lwip-tcpip/lwip}

if [ -f "$DIR/src/core/tcp.c" ]; then
  echo "lwIP already present in $DIR ($(git -C "$DIR" describe --tags 2>/dev/null || echo unknown))"
  exit 0
fi

mkdir -p "$(dirname "$DIR")"
echo "cloning $REPO @ $TAG -> $DIR"
git clone --depth 1 --branch "$TAG" "$REPO" "$DIR"
echo "done: $(git -C "$DIR" describe --tags)"
echo "now run: make"

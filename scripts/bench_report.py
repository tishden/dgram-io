#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Markdown tables from a bench_matrix results directory.

    scripts/bench_report.py bench/results/<run>

One table of p50 / p99 round trip (us) per backend and offered rate, from
kernel.csv + dpdk.csv. A case that did not keep up -- achieved below 99% of
offered, or more than 0.1% of the measured datagrams lost -- prints as
"sat. <achieved>k, <loss>%" instead of a latency: past that point the number
is the depth of a queue, not a property of the path. A stream backend
(tcp*) cannot lose data and stay connected, so loss there prints as
"broken, <loss>% lost": the connection failed, it was not overloaded.
"""
import csv
import os
import sys

ORDER = ["dpdk", "tcp-dpdk", "xdp", "xdp-copy", "tcp-xdp", "tcp-xdp-copy",
         "udp", "tcp", "uring", "uring-sqpoll"]


def rows(path):
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


def sustained(r):
    return (float(r["achieved_mps"]) >= 0.99 * float(r["offered_mps"])
            and float(r["lost_pct"]) <= 0.1)


def rate_label(v):
    v = int(float(v))
    return f"{v / 1e6:g}M" if v >= 1_000_000 else f"{v // 1000}k"


def cell(r):
    if r is None:
        return "no result"
    if sustained(r):
        return f"{float(r['p50_us']):.1f} / {float(r['p99_us']):.1f}"
    if r["label"].startswith("tcp") and float(r["lost_pct"]) > 0.1:
        return f"broken, {float(r['lost_pct']):.0f}% lost"
    return (f"sat. {float(r['achieved_mps']) / 1000:.0f}k, "
            f"{float(r['lost_pct']):.1f}%")


def main(d):
    data = rows(os.path.join(d, "kernel.csv")) + rows(os.path.join(d, "dpdk.csv"))
    rates = sorted({int(float(r["offered_mps"])) for r in data})
    by = {(r["label"], int(float(r["offered_mps"]))): r for r in data}
    labels = [k for k in ORDER if any(l == k for l, _ in by)]
    print("| backend | " + " | ".join(rate_label(x) for x in rates) + " |")
    print("| --- |" + " --- |" * len(rates))
    for k in labels:
        print(f"| {k} | " + " | ".join(cell(by.get((k, x))) for x in rates) + " |")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")

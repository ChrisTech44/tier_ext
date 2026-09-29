#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Summarize benchmark CSVs (bench/results/*.csv) as Markdown tables:
mean, standard deviation and number of runs per configuration.

    bench/summarize.py bench/results/overhead.csv [...]
"""
import csv
import statistics
import sys
from collections import OrderedDict

KEYS = {
    "overhead": (["kernel", "setup", "workload"], ["seconds"]),
    "filesearch": (["engine", "policy"], ["seconds", "pgscan", "pgsteal", "refaults"]),
    "ycsb": (["engine", "workload", "policy"], ["ops_per_s", "read_avg_ns", "read_p99_ns", "refaults"]),
}


def fmt(vals):
    vals = [v for v in vals if v is not None]
    if not vals:
        return "-"
    m = statistics.mean(vals)
    sd = statistics.stdev(vals) if len(vals) > 1 else 0.0
    if m >= 1000:
        return f"{m:,.0f} ± {sd:,.0f}"
    return f"{m:.2f} ± {sd:.2f}"


def num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def main(paths):
    rows = []
    for p in paths:
        with open(p) as f:
            rows += [r for r in csv.DictReader(l for l in f if l.strip()) if r.get("bench") not in (None, "bench")]
    by_bench = OrderedDict()
    for r in rows:
        by_bench.setdefault(r["bench"], []).append(r)
    for bench, rs in by_bench.items():
        keys, metrics = KEYS[bench]
        groups = OrderedDict()
        for r in rs:
            groups.setdefault(tuple(r[k] for k in keys), []).append(r)
        print(f"### {bench}\n")
        print("| " + " | ".join(keys + metrics + ["n"]) + " |")
        print("|" + "---|" * (len(keys) + len(metrics) + 1))
        for k, g in groups.items():
            cells = list(k) + [fmt([num(r[m]) for r in g]) for m in metrics] + [str(len(g))]
            print("| " + " | ".join(cells) + " |")
        print()


if __name__ == "__main__":
    main(sys.argv[1:])

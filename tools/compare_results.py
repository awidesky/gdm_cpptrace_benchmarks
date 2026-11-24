#!/usr/bin/env python3
"""Compare benchmark result dirs side by side.

    python tools/compare_results.py <parent dir>          # every subdir that has warm.tsv
    python tools/compare_results.py <dir> <dir> ...       # these result dirs

Writes a markdown table to stdout (and to <parent>/comparison.md with one parent dir).
Per result dir and optimization level:
  getCalledGLfunctionName C4 (labeling, the frequent path): old / new / cache hit / cache miss
  printStackTrace C1 (error output): old / new
  first call in a fresh process (cold, C4): old / new
  capture only (generate_raw_trace, from the API) of 5 and 21 frames: --frames C4 output
Values: µs (cold: ms); median over the dir's runs of each run's median (C4: its 7 sites).
"""
import csv
import os
import re
import statistics as st
import sys
from collections import defaultdict


def per_run_median(path, kind_scale):
    """(opt, case, api, variant) -> median over runs of the per-run median of total_med."""
    if not os.path.exists(path):
        return {}
    by_run = defaultdict(lambda: defaultdict(list))
    with open(path, encoding="utf-8") as f:
        for r in csv.DictReader(f, delimiter="\t"):
            key = (r["opt"], r["case"], r["api"], r["variant"])
            by_run[key][r.get("run", "0")].append(float(r["total_med"]))
    return {k: st.median(st.median(v) for v in runs.values()) / kind_scale for k, runs in by_run.items()}


def capture_costs(d):
    """opt -> {frames: ns} from frames-<opt>-C4.txt (or any *frames*C4*.txt naming the opt)."""
    out = defaultdict(dict)
    for name in os.listdir(d):
        m = re.search(r"frames.*C4", name)
        if not m or not name.endswith(".txt"):
            continue
        with open(os.path.join(d, name), encoding="utf-8", errors="replace") as f:
            for ln in f:
                p = ln.rstrip("\r\n").split("\t")
                # F <opt> <trigger> capture <n> frames <ns> ns
                if len(p) >= 6 and p[0] == "F" and p[3] == "capture":
                    out[p[1]][int(p[4].split()[0])] = float(p[5].split()[0])
    return out


def fmt(x, nd=1):
    return "-" if x is None or x != x else f"{x:.{nd}f}"


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    parent = None
    if len(args) == 1 and not os.path.exists(os.path.join(args[0], "warm.tsv")):
        parent = args[0]
        dirs = sorted(os.path.join(parent, n) for n in os.listdir(parent)
                      if os.path.exists(os.path.join(parent, n, "warm.tsv")))
    else:
        dirs = args
    lines = ["| result | opt | getCalled C4 old | new | cache hit | cache miss | print C1 old | new "
             "| cold C4 old (ms) | new (ms) | capture 5 fr (µs) | capture 21 fr (µs) |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for d in dirs:
        warm = per_run_median(os.path.join(d, "warm.tsv"), 1000.0)
        cold = per_run_median(os.path.join(d, "cold.tsv"), 1e6)
        cap = capture_costs(d)
        opts = sorted({k[0] for k in warm}, key=lambda o: ["O0", "O1", "O2", "O3", "Os"].index(o)
                      if o in ["O0", "O1", "O2", "O3", "Os"] else 9)
        for o in opts:
            g = lambda c, a, v: warm.get((o, c, a, v))
            c5 = cap.get(o, {}).get(5)
            c21 = cap.get(o, {}).get(21)
            lines.append("| " + " | ".join([
                os.path.basename(os.path.normpath(d)), o,
                fmt(g("C4", "getCalled", "old")), fmt(g("C4", "getCalled", "new")),
                fmt(g("C4", "getCalled", "cache_hit")), fmt(g("C4", "getCalled", "cache_miss")),
                fmt(g("C1", "print", "old")), fmt(g("C1", "print", "new")),
                fmt(cold.get((o, "C4", "getCalled", "old")), 2), fmt(cold.get((o, "C4", "getCalled", "new")), 2),
                fmt(c5 / 1000 if c5 is not None else None, 2), fmt(c21 / 1000 if c21 is not None else None, 2),
            ]) + " |")
    text = "\n".join(lines) + "\n"
    sys.stdout.write(text)
    if parent:
        with open(os.path.join(parent, "comparison.md"), "w", encoding="utf-8") as f:
            f.write("# Comparison (µs per call; cold in ms)\n\n" + text)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Analyse a captured F1 HID report log and work out the report layout.

Reads the log written by f1_hid_probe.py (one line per report:
"<seconds> <hex bytes>") and reports:

  * how many reports, and how many distinct byte-lengths
  * per-byte statistics (distinct values, min, max, times changed)
  * a timeline of which byte offsets changed, so knob sweeps and button
    presses can be told apart
  * candidate 16-bit little-endian fields, and how they move

Usage:
    python tools/f1_hid_analyze.py tools/f1_reports_raw.log
"""
import sys
from collections import defaultdict


def load(path):
    reports = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            parts = line.split()
            if len(parts) < 2:
                continue
            try:
                t = float(parts[0])
            except ValueError:
                continue
            try:
                data = bytes(int(b, 16) for b in parts[1:])
            except ValueError:
                continue
            reports.append((t, data))
    return reports


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "tools/f1_reports_raw.log"
    reports = load(path)
    if not reports:
        sys.exit(f"no reports in {path}")
    print(f"{len(reports)} reports from {path}")

    lengths = defaultdict(int)
    for _, d in reports:
        lengths[len(d)] += 1
    print(f"report lengths: {dict(lengths)}")

    ids = defaultdict(int)
    for _, d in reports:
        ids[d[0]] += 1
    print(f"report ids: {dict(ids)}")

    width = max(len(d) for _, d in reports)
    print(f"\n=== per-byte statistics (index: distinct min max changed) ===")
    changed = [0] * width
    for i in range(1, len(reports)):
        a, b = reports[i - 1][1], reports[i][1]
        for j in range(min(len(a), len(b))):
            if a[j] != b[j]:
                changed[j] += 1
    for j in range(width):
        vals = [d[j] for _, d in reports if len(d) > j]
        if not vals:
            continue
        print(f"  byte {j:2d}: distinct={len(set(vals)):4d} min={min(vals):3d} "
              f"max={max(vals):3d} changed={changed[j]:4d}")

    print("\n=== 16-bit LE candidate fields ===")
    for j in range(0, width - 1):
        vals = [(d[j] | (d[j + 1] << 8)) for _, d in reports if len(d) > j + 1]
        lo, hi = min(vals), max(vals)
        mono = sum(1 for a, b in zip(vals, vals[1:]) if b > a)
        down = sum(1 for a, b in zip(vals, vals[1:]) if b < a)
        print(f"  bytes {j:2d},{j + 1:2d}: min={lo:5d} max={hi:5d} "
              f"distinct={len(set(vals)):4d} up={mono:4d} down={down:4d}")

    print("\n=== timeline: changed byte offsets (runs collapsed) ===")
    prev = None
    run = []
    for t, d in reports:
        if prev is None:
            prev = d
            continue
        diff = tuple(j for j in range(min(len(d), len(prev))) if d[j] != prev[j])
        if not diff:
            continue
        if run and run[-1][0] == diff:
            run[-1][2] = t
            run[-1][3] += 1
        else:
            run.append([diff, t, t, 1])
        prev = d
    for diff, t0, t1, n in run[:200]:
        print(f"  {t0:8.3f}-{t1:8.3f}s  x{n:4d}  changed bytes {list(diff)}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""DUMP THE RATE, FRAME BY FRAME, AND SEE WHAT IT IS DOING.

The board answered 125.00, then 131.00, then 125.00 again.  Two numbers,
six BPM apart, and nothing in between.  A PLL that slews 0.5 % a search
cannot jump six BPM in one step, so the grid is being pulled by two
different winners and the rate is being dragged between them.

This dumps the rate trace so the shape of the drag is visible, and it
finds the windows where the rate moves the most so the worst case can
be looked at first.

    python tools/ar_sim/_fw_trace.py --limit 30
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402


def show(name, log, worst=None, n=60):
    log = np.array(log)
    if worst is not None:
        log = log[worst:worst + n]
    print("%s  n=%d  min=%.2f max=%.2f  distinct(0.25)=%d"
          % (name, len(log), log.min(), log.max(),
             len(set(np.round(log / 0.25)))))
    # the trace, 20 samples a line
    for i in range(0, len(log), 20):
        print("   %6d  %s" % (i, " ".join("%6.2f" % v for v in log[i:i + 20])))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=30)
    ap.add_argument("--lo", type=float, default=80.0)
    ap.add_argument("--hi", type=float, default=320.0)
    args = ap.parse_args()

    for k in _cache.keys()[:args.limit]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, args.lo, args.hi)
        if len(odf) < 1200:
            continue
        t = P.Tracker()
        t.narrow_bpm = 2.0
        t.collapse_trigger = True
        t.full_every = 32
        t.run(odf)
        log = np.array(t.bpm_log)
        if len(log) < 400:
            continue
        half = log[len(log) // 2:]
        half = half[half > 0]
        if len(half) < 100:
            continue
        jumps = int(np.sum(np.abs(np.diff(half)) > 0.5))
        if jumps < 5:
            continue
        print()
        print("=== %s   jumps=%d  std=%.3f" % (k[:60], jumps,
                                               float(np.std(half))))
        show("  narrow+net32", log, worst=len(log) // 2)

        t2 = P.Tracker()
        t2.narrow_bpm = 0.0
        t2.run(odf)
        log2 = np.array(t2.bpm_log)
        half2 = log2[len(log2) // 2:]
        half2 = half2[half2 > 0]
        print("  whole: jumps=%d std=%.3f"
              % (int(np.sum(np.abs(np.diff(half2)) > 0.5)), float(np.std(half2))))
        show("  whole", log2, worst=len(log2) // 2)

    return 0


if __name__ == "__main__":
    sys.exit(main())

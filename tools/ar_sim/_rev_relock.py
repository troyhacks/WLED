#!/usr/bin/env python3
"""REVIEW CHECK: the re-lock measurement the firmware quotes.

The firmware says: "the re-lock measured 16.6 s against 14.1 s without the
veto".  This measures the re-lock with the clamp and with the shipped
proportional response, so the number can be traced to the response it was
measured on.

    python -u tools/ar_sim/_rev_relock.py --windows 12 --pairs 8
"""

import argparse
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402

TOL_BPM = 1.0
HOLD = 281


def make(margin, gain):
    t = P.Tracker()
    t.narrow_bpm = 2.0
    t.search_every = 4
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = 32
    t.net_margin = margin
    t.pll_gain = gain
    t.exact_kernel = True
    return t


def reference_bpm(odf):
    t = make(0.0, 0.10)
    t.run(odf)
    return t.bpm if t.locked and t.bpm > 0 else None


def relock_time(t, cut, ref):
    log = t.bpm_log
    n = len(log)
    first = None
    for f in range(cut, n):
        if t.locked and abs(log[f] - ref) <= TOL_BPM:
            first = f
            break
    if first is None:
        return None
    for f in range(first, min(n, first + HOLD)):
        if not t.locked or abs(log[f] - ref) > 2.0 * TOL_BPM:
            return None
    return (first - cut) / P.RATE


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=12)
    ap.add_argument("--pairs", type=int, default=8)
    args = ap.parse_args()

    t0 = time.time()
    items = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, 80.0, 320.0)
        if len(odf) < 1200:
            continue
        items.append((k, odf))
    print("%d windows  %.0f s" % (len(items), time.time() - t0), flush=True)

    pairs = []
    for i in range(len(items)):
        bpm = reference_bpm(items[i][1])
        if bpm is None:
            continue
        for j in range(i + 1, len(items)):
            bpm2 = reference_bpm(items[j][1])
            if bpm2 is None or abs(bpm2 - bpm) < 6.0:
                continue
            pairs.append((items[i], items[j], bpm2))
            break
        if len(pairs) >= args.pairs:
            break
    print("%d pairs  %.0f s" % (len(pairs), time.time() - t0), flush=True)

    for gain in (0.0, 0.10):
        for margin in (0.0, 0.20):
            times, never = [], 0
            for (a, b, ref) in pairs:
                t = make(margin, gain)
                cut = len(a[1])
                t.run(np.concatenate((a[1], b[1])))
                r = relock_time(t, cut, ref)
                times.append(999.0 if r is None else r)
                if r is None:
                    never += 1
            arr = np.array(times)
            print("   gain %.2f  margin %.2f   re-lock median %6.2f s   worst %6.2f"
                  "   never %d" % (gain, margin, float(np.median(arr)),
                                  float(arr.max()), never), flush=True)
        print()

    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

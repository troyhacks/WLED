#!/usr/bin/env python3
"""THE RANGE, BY THE ERROR CLASS - the measurement the range question needs.

THE METRIC
----------
A beat-onset hit rate says whether the tracker is PHASED right.  The range
question asks something else: given that the tracker's tempo is wrong,
HOW is it wrong?  A track read at twice its speed is a range problem: the
band was too wide and the octave was inside it.  A track read at 3:2 is a
3:2 problem.  A track read 6 % slow is a resolution problem and no range
will fix it.

So this classifies every estimate against the oracle's tempo:

    ratio = est_bpm / oracle_bpm
    ratio in [0.97, 1.03]     : RIGHT
    ratio near 2.0           : OCTAVE (the band was too wide)
    ratio near 0.5           : HALF (the half-time won)
    ratio near 1.5 or 0.667  : THREE:TWO
    ratio near 1.25 or 0.8   : FOUR:THREE
    anything else            : WRONG

And it does it per band, with and without the prior, so the two
mechanisms can be compared on the same music.

    python tools/ar_sim/_tempo_ratio.py --limit 300
"""

import argparse
import json
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402
from _tempo_range import configure, oracle         # noqa: E402


def classify(r):
    if r <= 0:
        return "none"
    if 0.97 <= r <= 1.03:
        return "right"
    for (target, name) in ((2.0, "octave"), (0.5, "half"),
                           (1.5, "3:2"), (0.6667, "2:3"),
                           (1.25, "4:3"), (0.8, "3:4"),
                           (1.3333, "4:3"), (0.75, "3:4")):
        if abs(r - target) < 0.06:
            return name
    return "other"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=300)
    ap.add_argument("--lo", type=float, default=40.0)
    ap.add_argument("--hi", type=float, default=160.0)
    args = ap.parse_args()

    bands = [
        ("90-175 +prior", 90.0, 175.0, 0.6),
        ("90-175 noprior", 90.0, 175.0, 0.0),
        ("100-176 +prior", 100.0, 176.0, 0.6),
        ("110-176 +prior", 110.0, 176.0, 0.6),
        ("118-176 +prior", 118.0, 176.0, 0.6),
        ("120-176 +prior", 120.0, 176.0, 0.6),
        ("118-176 noprior", 118.0, 176.0, 0.0),
        ("70-140 +prior", 70.0, 140.0, 0.6),
        ("70-176 +prior", 70.0, 176.0, 0.6),
        ("70-176 noprior", 70.0, 176.0, 0.0),
    ]

    keys = _cache.keys()[:args.limit]
    truths = []
    for k in keys:
        o = oracle(k)
        if o is None:
            continue
        truths.append((k, o[0]))
    print("%d windows with an oracle" % len(truths))

    t0 = time.time()
    odfs = {}
    for (k, _) in truths:
        x = _cache.load(k)
        if x is None:
            continue
        odfs[k] = B.firmware_odf(x, args.lo, args.hi)
    print("odf: %.0f s" % (time.time() - t0))
    tb = np.array([t[1] for t in truths])
    print("oracle bpm: min %.1f max %.1f median %.1f  (n=%d)"
          % (tb.min(), tb.max(), float(np.median(tb)), len(tb)))
    print()

    for (label, lo, hi, pw) in bands:
        kinds = {}
        ratios = []
        for (k, tbpm) in truths:
            odf = odfs.get(k)
            if odf is None:
                continue
            configure(lo=lo, hi=hi, prior_weight=pw)
            t = P.Tracker()
            t.run(odf)
            if t.bpm <= 0:
                kinds["none"] = kinds.get("none", 0) + 1
                continue
            r = t.bpm / tbpm
            ratios.append(r)
            c = classify(r)
            kinds[c] = kinds.get(c, 0) + 1
        n = len(ratios) + kinds.get("none", 0)
        parts = " ".join("%s %d" % (kk, vv)
                         for kk, vv in sorted(kinds.items(),
                                              key=lambda t: -t[1]))
        rs = np.array(ratios) if ratios else np.array([0.0])
        print("%-20s n=%3d  %-52s  median ratio %.4f  |r-1| med %.3f"
              % (label, n, parts, float(np.median(rs)),
                 float(np.median(np.abs(rs - 1.0)))))
    return 0


if __name__ == "__main__":
    sys.exit(main())

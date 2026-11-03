#!/usr/bin/env python3
"""DOES THE COLLAPSE BRANCH EVER FIRE?

Instruments the port's sweep_step so it counts, per sweep:
  * how many sweeps ran NARROW and how many WHOLE-BAND
  * how many finished with sweep_best_score < min_score (the collapse test)
  * of those, how many were narrow (the collapse branch) and how many were
    whole-band (the fullSearchFailed branch)

    python tools/ar_sim/_fw_collapsetest.py --windows 40
"""

import argparse
import os
import sys
from collections import Counter

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=40)
    ap.add_argument("--band-lo", type=float, default=80.0)
    ap.add_argument("--band-hi", type=float, default=320.0)
    ap.add_argument("--tols", default="30,70,150")
    args = ap.parse_args()

    tols = [float(t) for t in args.tols.split(",")]
    for tol in tols:
        P.TOL_MS = tol
        c = Counter()
        minsc = []
        nwin = 0
        for k in _cache.keys()[:args.windows]:
            x = _cache.load(k)
            if x is None:
                continue
            odf = B.firmware_odf(x, args.band_lo, args.band_hi)
            if len(odf) < 1200:
                continue
            t = P.Tracker()
            orig = t.sweep_step

            def wrapped(orig=orig, t=t, c=c):
                was_narrow = t.sweep_was_narrow
                before_score = t.sweep_best_score
                n_before = t.sweep_index
                orig()
                if t.sweep_index > n_before:
                    c["narrow" if was_narrow else "whole"] += 1
                    if t.sweep_best_score < t.min_score:
                        c["collapse_narrow" if was_narrow
                          else "failed_full"] += 1
                        minsc.append(float(t.sweep_best_score))
            t.sweep_step = wrapped
            t.run(odf)
            nwin += 1

        print("=== tol %.0f ms   windows: %d" % (tol, nwin))
        print("   narrow sweeps:        %6d" % c["narrow"])
        print("   whole-band sweeps:    %6d" % c["whole"])
        print("   COLLAPSE branch fired:%6d  (narrow, score < %.2f)"
              % (c["collapse_narrow"], P.MIN_SCORE))
        print("   fullSearchFailed set: %6d  (whole-band, score < %.2f)"
              % (c["failed_full"], P.MIN_SCORE))
        if minsc:
            print("   lowest sweep score:    %.4f" % min(minsc))
    return 0


if __name__ == "__main__":
    sys.exit(main())

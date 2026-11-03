#!/usr/bin/env python3
"""THE COLLAPSE BRANCH, ON THE CURRENT SHIPPED CODE.

The constant AR_TEMPO_MIN_SCORE is now 0.35 with a hysteresis of
AR_TEMPO_COLLAPSE_COUNT = 4 consecutive searches below the floor.

So the question is: does the narrow search's score fall below 0.35 four
searches in a row on real audio?  And does the whole-band search ever fall
below 0.35 (the fullSearchFailed branch)?  And does arTempoValid() -- now
gated on the collapse count, not on the score -- ever go false?

This instruments the port's sweep_step and counts:
  * sweeps whose best score was below min_score
  * the longest run of CONSECUTIVE below-floor sweeps
  * whether the collapse branch fired
  * whether fullSearchFailed was set
  * whether valid ever went false after the first lock

    python tools/ar_sim/_fw_collapsetest2.py --windows 40
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
    ap.add_argument("--tols", default="70")
    args = ap.parse_args()

    for tol in [float(t) for t in args.tols.split(",")]:
        P.TOL_MS = tol
        c = Counter()
        maxrun = 0
        worst = 1.0
        nwin = 0
        valid_false_after_lock = 0
        for k in _cache.keys()[:args.windows]:
            x = _cache.load(k)
            if x is None:
                continue
            odf = B.firmware_odf(x, args.band_lo, args.band_hi)
            if len(odf) < 1200:
                continue
            t = P.Tracker()
            orig = t.sweep_step
            state = {"run": 0}

            def wrapped(orig=orig, t=t, c=c, state=state):
                was_narrow = t.sweep_was_narrow
                n_before = t.sweep_index
                orig()
                if t.sweep_index > n_before:
                    c["narrow" if was_narrow else "whole"] += 1
                    if t.sweep_best_score < t.min_score:
                        c["below_narrow" if was_narrow
                          else "below_full"] += 1
                        state["run"] += 1
                        state["maxrun"] = max(state.get("maxrun", 0),
                                               state["run"])
                        if was_narrow and t.collapse_count >= t.collapse_count_max:
                            c["COLLAPSE"] += 1
                    else:
                        state["run"] = 0
                    state["worst"] = min(state.get("worst", 1.0),
                                        t.sweep_best_score)
            t.sweep_step = wrapped
            t.run(odf)
            maxrun = max(maxrun, state.get("maxrun", 0))
            if t.full_search_failed:
                c["fullSearchFailed"] += 1
            sc = np.array(t.score_log)
            if len(sc):
                state["worst"] = min(state.get("worst", 1.0), float(sc.min()))
                worst = min(worst, state["worst"])
            nwin += 1

        print("=== tol %.0f ms   MIN_SCORE %.2f   COLLAPSE_COUNT %d   "
              "windows: %d" % (tol, P.MIN_SCORE, P.COLLAPSE_COUNT, nwin))
        print("   narrow sweeps:            %6d" % c["narrow"])
        print("   whole-band sweeps:        %6d" % c["whole"])
        print("   narrow below the floor:   %6d" % c["below_narrow"])
        print("   whole below the floor:    %6d" % c["below_full"])
        print("   COLLAPSE branch fired:    %6d" % c["COLLAPSE"])
        print("   fullSearchFailed set:     %6d  (windows: %d)"
              % (c["fullSearchFailed"], nwin))
        print("   longest run of consecutive below-floor sweeps: %d" % maxrun)
        print("   lowest sweep score seen:  %.4f" % worst)
    return 0


if __name__ == "__main__":
    sys.exit(main())

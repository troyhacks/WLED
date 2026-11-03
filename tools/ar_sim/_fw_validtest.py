#!/usr/bin/env python3
"""DOES THE PUBLISHED VALID FLAG EVER GO FALSE?

arTempoValid() on the shipped code is:

    return arTempo.locked && arTempo.collapseCount < AR_TEMPO_COLLAPSE_COUNT;

so the published flag goes false exactly when the collapse counter reaches
AR_TEMPO_COLLAPSE_COUNT (4) while the tracker is locked.

This drives the port over the corpus and, after every completed search,
records (locked, collapse_count) - the state the published valid flag is
computed from - and counts how long the flag spends false.

    python tools/ar_sim/_fw_validtest.py --windows 40
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

    for tol in [float(t) for t in args.tols.split(",")]:
        P.TOL_MS = tol
        c = Counter()
        win_invalid = 0
        sweeps_invalid = 0
        sweeps_total = 0
        maxrun = 0
        maxcc = 0
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
            state = {"run": 0, "invalid": 0}

            def wrapped(orig=orig, t=t, c=c, state=state):
                n_before = t.sweep_index
                orig()
                if t.sweep_index > n_before:
                    # the search that just finished
                    sweeps_total = state["sweeps_total"] = \
                        state.get("sweeps_total", 0) + 1
                    valid = (t.locked
                             and t.collapse_count < t.collapse_count_max)
                    if not valid and t.locked:
                        c["invalid_while_locked"] += 1
                        state["run"] += 1
                        state["maxrun"] = max(state.get("maxrun", 0),
                                              state["run"])
                        state["invalid"] = 1
                    else:
                        state["run"] = 0
                    maxcc = state["maxcc"] = max(state.get("maxcc", 0),
                                                 t.collapse_count)
            t.sweep_step = wrapped
            t.run(odf)
            nwin += 1
            maxrun = max(maxrun, state.get("maxrun", 0))
            maxcc = max(maxcc, state.get("maxcc", 0))
            sweeps_total = state.get("sweeps_total", 0)
            if state.get("invalid"):
                win_invalid += 1
            if t.full_search_failed:
                c["full_search_failed"] += 1
        print("=== tol %.0f ms   MIN_SCORE %.2f   COLLAPSE_COUNT %d   "
              "windows: %d" % (tol, P.MIN_SCORE, P.COLLAPSE_COUNT, nwin))
        print("   searches where the published valid flag was FALSE while "
              "locked: %d / %d" % (c["invalid_while_locked"], sweeps_total))
        print("   windows that saw valid == false at least once: %d / %d"
              % (win_invalid, nwin))
        print("   longest run of consecutive invalid searches: %d" % maxrun)
        print("   largest collapse count reached: %d" % maxcc)
        print("   windows where fullSearchFailed was set: %d / %d"
              % (c["full_search_failed"], nwin))
    return 0


if __name__ == "__main__":
    sys.exit(main())

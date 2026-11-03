#!/usr/bin/env python3
"""THE STRUCTURAL FLOOR OF THE COMB SCORE, DIRECTLY.

The corpus is dense house music at one tempo.  The score's floor is really a
property of the KERNEL: the box covers 2w+1 of the 64 phase bins, and the
bins it covers are a fixed share of the phase circle.  A RANDOM onset set --
music with no beat at all -- therefore earns a floor, and the floor is what
the collapse test would actually see when the music changes to something
rhythmless.

This asks the port's own comb score directly:

  * the WHOLE-BAND best: the best of all 341 candidates.  This is the score
    a whole-band search reports, so it is the score the "found nothing"
    branch would see.
  * the NARROW best: the best of the 17 candidates around a given rate.
    This is the score the collapse branch would see.

against random onsets at each tolerance the board allows, at the band edges
and in the middle.

    python tools/ar_sim/_fw_structfloor.py
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _fw_port as P                               # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--npeaks", type=int, default=12)
    ap.add_argument("--seeds", type=int, default=12)
    ap.add_argument("--tols", default="30,50,70,100,150")
    args = ap.parse_args()

    band = np.arange(P.BPM_LO, P.BPM_HI + 1e-6, P.CAND_STEP)

    print("%d random onsets, %d seeds.  The score's floor, and the "
          "narrow best at a WRONG rate." % (args.npeaks, args.seeds))
    print()
    for tol in [float(t) for t in args.tols.split(",")]:
        P.TOL_MS = tol
        whole = []
        narrow_at = {}
        for seed in range(args.seeds):
            rng = np.random.default_rng(seed)
            # random onset times, random-ish strengths
            frames = np.sort(rng.uniform(0, P.COMB_FRAMES, args.npeaks))
            strs = rng.uniform(0.2, 1.0, args.npeaks)
            t = P.Tracker()
            s, _ = t.comb_scores_all(frames, strs, band)
            whole.append(float(s.max()))
            # the narrow best around a rate the music is NOT at
            for target in (90.0, 100.0, 125.0, 150.0, 175.0):
                i = int(round((target - P.BPM_LO) / P.CAND_STEP))
                lo = max(0, min(len(band) - 17, i - 8))
                sub, _ = t.comb_scores_all(frames, strs, band[lo:lo + 17])
                narrow_at.setdefault(target, []).append(float(sub.max()))
        print("tol %5.0f ms" % tol)
        print("   WHOLE-BAND best (341 cands): min %.4f  median %.4f  max %.4f"
              % (min(whole), float(np.median(whole)), max(whole)))
        for target in sorted(narrow_at):
            v = narrow_at[target]
            print("   NARROW best around %5.1f BPM      : min %.4f  median %.4f"
                  % (target, min(v), float(np.median(v))))
    return 0


if __name__ == "__main__":
    sys.exit(main())

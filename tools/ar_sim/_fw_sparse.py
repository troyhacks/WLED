#!/usr/bin/env python3
"""THE SPARSE-ONSET BOUND ON THE SCORE.

The corpus is dense house music.  The narrow search's score has a STRUCTURAL
floor: it is the best of 17 candidates, each of which puts a box of the
tolerance's width over the phase circle, and the box's share of the circle
sets the score a RANDOM onset set would earn.

So: with only AR_TEMPO_MIN_PEAKS onsets in the window - the fewest a search
may run on - what does the score come to?  This drives the port with a
synthetic ODF that has a controllable number of onsets, at each tolerance the
board allows, and reports the lowest score the narrow search ever sees.

    python tools/ar_sim/_fw_sparse.py
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _fw_port as P                               # noqa: E402


def synth_odf(npeaks, nframes=6000, seed=1):
    """An ODF with npeaks sharp peaks and nothing else."""
    rng = np.random.default_rng(seed)
    odf = np.zeros(nframes, dtype=np.float32)
    pos = rng.choice(np.arange(10, nframes - 10), size=npeaks, replace=False)
    for p in pos:
        odf[p] = 1.0
    return odf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=6000)
    args = ap.parse_args()

    for tol in (30.0, 70.0, 150.0):
        P.TOL_MS = tol
        print("=== tol %.0f ms" % tol)
        for npeaks in (8, 10, 12, 16, 24, 40):
            lows = []
            for seed in range(1, 9):
                t = P.Tracker()
                t.run(synth_odf(npeaks, args.frames, seed))
                sc = np.array(t.score_log)
                if len(sc):
                    lows.append(float(sc.min()))
            if lows:
                print("   %2d onsets   lowest score seen: %.4f   (median of "
                      "seeds %.4f)"
                      % (npeaks, min(lows), float(np.median(lows))))
    return 0


if __name__ == "__main__":
    sys.exit(main())

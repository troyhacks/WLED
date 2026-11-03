#!/usr/bin/env python3
"""THE COMB'S FLOOR.

What does the comb score when the rate is WRONG?

The kernel is a box of half-width tol (70 ms) over 64 phase bins, so at
128 BPM it is 21 bins wide - a third of the phase circle.  A wrong rate
smears the onsets around the circle, and a box a third of the circle wide
still catches a third of them.

So the score of a wrong rate is not zero, it is about a third.  And
AR_TEMPO_MIN_SCORE is 0.10.

This measures it: a clean beat train at one rate, scored at every candidate.
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _fw_port as P                               # noqa: E402


def train(bpm, seconds=12.0):
    period = 60.0 * P.RATE / bpm
    n = int(seconds * P.RATE / period)
    frames = np.array([i * period for i in range(n)], dtype=float)
    strengths = np.ones(n, dtype=float)
    return frames, strengths, float(strengths.sum())


def main():
    for exact in (False, True):
        t = P.Tracker()
        t.exact_kernel = exact
        print("tol %.0f ms   MIN_SCORE %.2f   bins %d   exact_kernel=%s"
              % (P.TOL_MS, P.MIN_SCORE, P.COMB_BINS, exact))
        print("%8s %8s %8s %8s" % ("true", "min", "max", "at"))
        for bpm in (95.0, 110.0, 128.0, 140.0, 160.0, 175.0):
            frames, strengths, total = train(bpm)
            rows = []
            for c in range(P.CAND_COUNT):
                cbpm = P.BPM_LO + c * P.CAND_STEP
                s, _ph = t.comb_score(frames, strengths, cbpm)
                rows.append((s, cbpm))
            rows.sort()
            print("%8.2f %8.3f %8.3f %8.2f   (worst wrong rate %.2f)"
                  % (bpm, rows[0][0], rows[-1][0], rows[-1][1], rows[0][1]))
        print()


if __name__ == "__main__":
    sys.exit(main())

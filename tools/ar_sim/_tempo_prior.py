#!/usr/bin/env python3
"""THE PRIOR, AND THE 4:3 RESIDUAL - the last two open questions.

WHAT THE LAST MEASUREMENT SAID
------------------------------
On the real corpus the prior decided everything and the band changed
nothing: every band from 70-140 to 70-176 gave the same 85 % right, and
switching the prior off dropped it to 22 %.  That is not the band working,
it is the corpus: the corpus is house music with a median of 127.8 BPM and
the prior is centred at 125.  The prior and the corpus's answer are the
same thing.

On tracks whose tempo is KNOWN - synthetic, spanning the band - the order
reverses:

    band          with the prior    without it
    90-175           45/72 (62 %)     47/72 (65 %)
    118-176          40/54 (74 %)     46/54 (85 %)   <- the prior HURTS
    70-140           38/72 (53 %)     58/72 (81 %)   <- and it hurts a lot

and the errors in the narrow band are NEVER octave and NEVER 3:2 - they
are all 4:3.  Which is the theorem's promise kept: a band with a margin of
+30 BPM has no octave competitor and no 3:2 competitor inside it.

So this settles two things:

  1. THE PRIOR.  Swept: how much weight, and centred where, inside the
     narrow band.  A prior centred at the band's middle is a tie-break; a
     prior centred at 125 is a house-music assumption.

  2. THE 4:3 RESIDUAL.  Which tracks produce it, and whether demoting the
     4:3 competitor - the way the digest's 3:2 tie-break demotes - fixes
     it.

    python tools/ar_sim/_tempo_prior.py
"""

import argparse
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _fw_port as P                               # noqa: E402
from _tempo_band_synth import fw_odf, classify     # noqa: E402
from _tempo2_sim import STYLES, make_track, _seed  # noqa: E402

BPMS = [118, 120, 124, 128, 132, 135, 140, 145, 150, 155, 160, 165, 170,
        174, 176]


def configure(lo, hi, prior_weight, prior_bpm, prior_sigma=0.35):
    P.BPM_LO = float(lo)
    P.BPM_HI = float(hi)
    P.CAND_STEP = 0.25
    P.CAND_COUNT = int(round((float(hi) - float(lo)) / 0.25)) + 1
    P.PRIOR_BPM = float(prior_bpm)
    P.PRIOR_SIGMA = float(prior_sigma)
    P.PRIOR_WEIGHT = float(prior_weight)


def build_odfs():
    odfs = {}
    for style in STYLES:
        for bpm in BPMS:
            x = make_track(bpm, seconds=16.0, style=style,
                           seed=_seed(style, bpm))
            odfs[(style, bpm)] = fw_odf(x)
    return odfs


def sweep(odfs, lo, hi, pw, pb, sigma=0.35):
    kinds = {}
    detail = []
    for style in STYLES:
        for bpm in BPMS:
            if not (lo <= bpm <= hi):
                continue
            odf = odfs[(style, bpm)]
            if len(odf) < 200:
                continue
            configure(lo, hi, pw, pb, sigma)
            t = P.Tracker()
            t.run(odf)
            c = classify(t.bpm / bpm if t.bpm > 0 else 0.0)
            kinds[c] = kinds.get(c, 0) + 1
            if c != "right":
                detail.append((style, bpm, t.bpm, c))
    return kinds, detail


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lo", type=float, default=118.0)
    ap.add_argument("--hi", type=float, default=176.0)
    args = ap.parse_args()

    print("building the ODFs...")
    odfs = build_odfs()
    mid = 0.5 * (args.lo + args.hi)

    print("\n=== the prior, inside the band %g-%g ===" % (args.lo, args.hi))
    print("%-28s %s" % ("prior", "right / n   error classes"))
    trials = [
        ("none", 0.0, mid),
        ("w0.1 @ mid %.1f" % mid, 0.1, mid),
        ("w0.2 @ mid %.1f" % mid, 0.2, mid),
        ("w0.4 @ mid %.1f" % mid, 0.4, mid),
        ("w0.6 @ mid %.1f" % mid, 0.6, mid),
        ("w0.2 @ 125", 0.2, 125.0),
        ("w0.6 @ 125 (shipped)", 0.6, 125.0),
        ("w0.2 @ 125 sig0.7", 0.2, 125.0),
    ]
    for (label, pw, pb) in trials:
        sigma = 0.7 if "sig0.7" in label else 0.35
        kinds, detail = sweep(odfs, args.lo, args.hi, pw, pb, sigma)
        n = sum(kinds.values())
        r = kinds.get("right", 0)
        parts = " ".join("%s %d" % (k, v) for k, v in sorted(kinds.items(),
                                                             key=lambda t: -t[1]))
        print("%-28s %3d/%3d (%3.0f%%)  %s" % (label, r, n,
                                                100.0 * r / n, parts))

    print("\n=== the 4:3 residual, with no prior ===")
    kinds, detail = sweep(odfs, args.lo, args.hi, 0.0, mid)
    print("%-10s %6s %9s %s" % ("style", "bpm", "est", "class"))
    for (style, bpm, est, c) in detail:
        print("%-10s %6d %9.2f %s   (est*3/4 = %.2f)"
              % (style, bpm, est, c, est * 3.0 / 4.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())

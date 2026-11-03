#!/usr/bin/env python3
"""The comb's bin count, on the FULL 54-track set, with the signed bias.

The 16-bin comb was chosen from a 30-track sample where the differences
between 12, 16 and 24 bins were inside the noise.  This is the larger sample
that settles it: six styles x nine tempi, the tempo taken from the
ESTIMATOR (not the truth), so the number is the one the firmware will see.

Run: python tools/ar_sim/_tempo17_comb.py
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _tempo5_lock_sim import RATE, odf_of, STYLES            # noqa: E402
from _tempo6_final_sim import spectral                       # noqa: E402
from _tempo16_pick import kick_frames, comb_from, err_ms, signed_err_beats  # noqa: E402

BS = list(range(2, 9))
TRUTHS = [90, 100, 110, 120, 128, 140, 150, 160, 174]

print("=== the phase comb's bin count, on the ESTIMATED tempo, 54 tracks ===")
print("%-6s %10s %10s %10s %13s %10s" %
      ("bins", "mean ms", "median ms", "max ms", "bias (beat)", "sd (beat)"))
for nbins in (8, 12, 16, 20, 24, 32):
    errs = []
    biases = []
    for style in STYLES:
        for bpm in TRUTHS:
            odf = odf_of(BS, style, bpm)
            est, _, _ = spectral(odf)
            P = 60.0 / est * RATE
            kicks = kick_frames(bpm, style)
            W = int(4.0 * RATE)
            centre = len(odf) - W
            p = comb_from(odf, P, W, nbins=nbins)
            if p is None:
                continue
            errs.append(np.mean(err_ms(p, P, kicks, centre)))
            biases.append(np.mean(signed_err_beats(p, P, kicks, centre)))
    errs = np.array(errs)
    biases = np.array(biases)
    print("%-6d %10.1f %10.1f %10.1f %13.4f %10.4f" %
          (nbins, np.mean(errs), np.median(errs), np.max(errs),
           np.mean(biases), np.std(biases)))
print()

print("=== the same, per bin count, the WORST three tracks (16 bins) ===")
rows = []
for style in STYLES:
    for bpm in TRUTHS:
        odf = odf_of(BS, style, bpm)
        est, _, _ = spectral(odf)
        P = 60.0 / est * RATE
        kicks = kick_frames(bpm, style)
        W = int(4.0 * RATE)
        centre = len(odf) - W
        p = comb_from(odf, P, W, nbins=16)
        if p is None:
            continue
        e = float(np.mean(err_ms(p, P, kicks, centre)))
        b = float(np.mean(signed_err_beats(p, P, kicks, centre)))
        rows.append((e, b, style, bpm, est))
rows.sort(reverse=True)
for e, b, style, bpm, est in rows[:8]:
    print("  %-9s %3d BPM  est %7.2f  error %6.1f ms  bias %+.4f beat" %
          (style, bpm, est, e, b))
print()

print("=== the window length, on the estimated tempo (16 bins) ===")
for secs in (2.0, 3.0, 4.0, 6.0, 8.0):
    errs = []
    biases = []
    for style in STYLES:
        for bpm in TRUTHS:
            odf = odf_of(BS, style, bpm)
            est, _, _ = spectral(odf)
            P = 60.0 / est * RATE
            kicks = kick_frames(bpm, style)
            W = int(secs * RATE)
            centre = len(odf) - W
            p = comb_from(odf, P, W, nbins=16)
            if p is None:
                continue
            errs.append(np.mean(err_ms(p, P, kicks, centre)))
            biases.append(np.mean(signed_err_beats(p, P, kicks, centre)))
    errs = np.array(errs)
    biases = np.array(biases)
    print("  %4.1f s  mean %6.1f ms  median %6.1f ms  max %6.1f ms  "
          "bias %+.4f  sd %.4f" %
          (secs, np.mean(errs), np.median(errs), np.max(errs),
           np.mean(biases), np.std(biases)))
print()

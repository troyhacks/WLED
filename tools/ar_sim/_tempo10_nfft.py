#!/usr/bin/env python3
"""Experiment 10: how big does the DFT grid have to be?

The running DFT's table is the design's biggest single allocation (a
cos table of NFFT floats) and its per-frame cost scales with the number
of bins in the band.  The band is 90-175 BPM, so NFFT sets both the bin
width and how many bins are scanned.

The zero-padded grid does NOT have to be 4096: the window is 1125 frames,
so NFFT=2048 is the smallest power of two that holds the window without
aliasing, and the parabolic interpolation on the log magnitude does not
need the bin to be narrower than the peak.

    python tools/ar_sim/_tempo10_nfft.py
"""

import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _tempo5_sim as t5                                            # noqa: E402
import _tempo3_sim as t3                                           # noqa: E402
from _tempo2_sim import STYLES                                      # noqa: E402

BPMS = (90, 100, 120, 128, 140, 150, 160, 174)
CACHE = {}


def set_nfft(n):
    t3.NFFT = n
    t3._TW = None
    t5.NFFT = n
    t5.BPM_PER_BIN = t5.RATE / n * 60.0
    t5.GK_MIN = int(math.floor(t5.GRID_LO_BPM / t5.BPM_PER_BIN)) - 2
    t5.GK_MAX = int(math.ceil(t5.GRID_HI_BPM / t5.BPM_PER_BIN)) + 2
    t5.GK_N = t5.GK_MAX - t5.GK_MIN + 1


def mags_for(style, bpm):
    key = (style, bpm, t5.NFFT)
    if key not in CACHE:
        x = t5.get_track(style, bpm, 0.0, 0.0)
        from _tempo3_sim import filter_designs
        odf = t5.odf_fast(x, filter_designs()["hp40_lp160x2"])
        CACHE[key] = (odf, t5.running_dft_grid(odf))
    return CACHE[key]


def main():
    for n in (1024, 2048, 4096, 8192):
        set_nfft(n)
        klo, khi = t5.band_bins(90.0, 175.0)
        wrong = []
        errs = []
        worst = 0.0
        for style in STYLES:
            for bpm in BPMS:
                odf, mags = mags_for(style, bpm)
                est, mag = t5.peak_strict(mags, klo, khi)
                est = t5.rational_slower(est, mag, mags, klo, khi, 3.0)
                e = abs(est - bpm)
                errs.append(e)
                worst = max(worst, e)
                if e >= 2.0:
                    wrong.append("%s/%d->%.1f" % (style, bpm, est))
        print("NFFT %5d  bin %5.3f BPM  bins %2d  wrong %d  mean|err| %6.3f "
              "max %6.3f  %s" %
              (n, t5.BPM_PER_BIN, khi - klo + 1, len(wrong),
               sum(errs) / len(errs), worst, " ".join(wrong)))


if __name__ == "__main__":
    main()

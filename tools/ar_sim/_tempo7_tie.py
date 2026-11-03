#!/usr/bin/env python3
"""Experiment 7 (fast): design the PEAK SELECTION rule from the data.

The strict band already removes the octave error (Dixon's 39%).  What is
left is the 3:2 and 4:3 family.  _tempo5_sim.py's `rational_slower`
demotes the winner to ANY lower peak within 3 dB whose ratio is p/q with
p,q <= 4.  Measured: that made the 70-176 band WORSE (5 wrong instead
of 1) and did nothing for 90-175 (1 wrong either way).

The ODF and the magnitudes are computed ONCE per track and every rule and
every band is evaluated from them.

    python tools/ar_sim/_tempo7_tie.py
"""

import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _tempo5_sim as t5                                            # noqa: E402
from _tempo2_sim import STYLES                                      # noqa: E402

BPMS = (90, 100, 120, 128, 140, 150, 160, 174)
CACHE = {}


def mags_for(style, bpm, design="hp40_lp160x2"):
    key = (style, bpm, design)
    if key not in CACHE:
        x = t5.get_track(style, bpm, 0.0, 0.0)
        from _tempo3_sim import filter_designs
        odf = t5.odf_fast(x, filter_designs()[design])
        CACHE[key] = (odf, t5.running_dft_grid(odf))
    return CACHE[key]


def _demote_to(winner, wmag, mags, klo, khi, ratios, tol_db):
    if tol_db <= 0.0:
        return winner
    thr = wmag / (10.0 ** (tol_db / 20.0))
    best = None
    for b, m in t5.local_peaks_strict(mags, klo, khi):
        if b >= winner - 1e-9:
            continue
        if m <= 0.0 or m < thr:
            continue
        r = winner / b
        for (p, q) in ratios:
            if abs(r - p / q) < 0.01 * (p / q):
                if best is None or b > best:
                    best = b
    return best if best is not None else winner


def rule_none(mags, klo, khi):
    bpm, mag = t5.peak_strict(mags, klo, khi)
    return bpm


def rule_32_3db(mags, klo, khi):
    bpm, mag = t5.peak_strict(mags, klo, khi)
    return _demote_to(bpm, mag, mags, klo, khi, [(3, 2)], 3.0)


def rule_32_1db(mags, klo, khi):
    bpm, mag = t5.peak_strict(mags, klo, khi)
    return _demote_to(bpm, mag, mags, klo, khi, [(3, 2)], 1.0)


def rule_32_43(mags, klo, khi):
    bpm, mag = t5.peak_strict(mags, klo, khi)
    return _demote_to(bpm, mag, mags, klo, khi, [(3, 2), (4, 3)], 3.0)


def rule_all4(mags, klo, khi):
    bpm, mag = t5.peak_strict(mags, klo, khi)
    return _demote_to(bpm, mag, mags, klo, khi,
                       [(p, q) for q in range(1, 5) for p in range(q + 1, 5)], 3.0)


def rule_prior120(mags, klo, khi, tau0=120.0, sigma=0.8):
    """The tallest peak, tilted by a log-normal prior centred on 120 BPM."""
    peaks = t5.local_peaks_strict(mags, klo, khi)
    if not peaks:
        bpm, _ = t5.peak_strict(mags, klo, khi)
        return bpm
    best = None
    bestv = None
    for b, m in peaks:
        w = math.log2(b / tau0) / sigma
        v = 20.0 * math.log10(m + 1e-30) - 0.5 * w * w
        if bestv is None or v > bestv:
            bestv = v
            best = b
    return best


RULES = {
    "none": rule_none,
    "32_3dB": rule_32_3db,
    "32_1dB": rule_32_1db,
    "32+43": rule_32_43,
    "all4": rule_all4,
    "prior120": rule_prior120,
}


def main():
    bands = [(90, 150), (90, 175), (100, 175), (120, 176), (70, 176)]
    # warm the cache once
    for style in STYLES:
        for bpm in BPMS:
            mags_for(style, bpm)
    print("cache warm: %d tracks" % len(CACHE))
    for (lo, hi) in bands:
        klo, khi = t5.band_bins(lo, hi)
        print("=" * 78)
        print("BAND %d-%d" % (lo, hi))
        for rname, rfn in RULES.items():
            wrong = []
            errs = []
            for style in STYLES:
                for bpm in BPMS:
                    odf, mags = mags_for(style, bpm)
                    est = rfn(mags, klo, khi)
                    e = abs(est - bpm)
                    errs.append(e)
                    if e >= 2.0:
                        wrong.append("%s/%d->%.1f" % (style, bpm, est))
            print("  %-8s wrong %2d  mean|err| %6.2f   %s" %
                  (rname, len(wrong), sum(errs) / len(errs), " ".join(wrong)))


if __name__ == "__main__":
    main()

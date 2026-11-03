#!/usr/bin/env python3
"""Experiment 12: the FINAL table - the front end x the grid x the rule.

Everything the firmware will use, in one run:
  * the 3:2-only tie-break (prefer the dotted-quarter partner within 3 dB,
    and NOTHING else - the 4:3 family is left alone)
  * the confidence features, for the tracks the rule gets right

    python tools/ar_sim/_tempo12_final.py
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


def odf_for(style, bpm, sections):
    key = (style, bpm, id(sections))
    if key not in CACHE:
        x = t5.get_track(style, bpm, 0.0, 0.0)
        CACHE[key] = t5.odf_fast(x, sections)
    return CACHE[key]


def tie_32(winner, wmag, mags, klo, khi, tol_db=3.0):
    """Demote to the 3:2 partner only. The 4:3 family is NOT demoted."""
    if tol_db <= 0.0:
        return winner
    thr = wmag / (10.0 ** (tol_db / 20.0))
    for b, m in t5.local_peaks_strict(mags, klo, khi):
        if b >= winner - 1e-9:
            continue
        if m <= 0.0 or m < thr:
            continue
        r = winner / b
        if abs(r - 1.5) < 0.015:
            return b
    return winner


def features(mags, klo, khi, kbest, mag_w, bpm_w):
    guard = [k - t5.GK_MIN for k in range(klo, khi + 1) if abs(k - kbest) > 3]
    bg = float(np.median(mags[guard])) if guard else 0.0
    snr = 20.0 * math.log10((mag_w + 1e-30) / (bg + 1e-30))
    peaks = t5.local_peaks_strict(mags, klo, khi)
    peaks.sort(key=lambda t: -t[1])
    other = [m for b, m in peaks if abs(b - bpm_w) > 2.0]
    margin = (20.0 * math.log10((mag_w + 1e-30) / (other[0] + 1e-30))
              if other else 40.0)
    return snr, margin


def run(name, sections, nfft, verbose=False):
    set_nfft(nfft)
    klo, khi = t5.band_bins(90.0, 175.0)
    wrong = []
    errs = []
    snrs = []
    margins = []
    for style in STYLES:
        for bpm in BPMS:
            odf = odf_for(style, bpm, sections)
            mags = t5.running_dft_grid(odf)
            est, mag = t5.peak_strict(mags, klo, khi)
            est = tie_32(est, mag, mags, klo, khi, 3.0)
            e = abs(est - bpm)
            errs.append(e)
            seg = mags[klo - t5.GK_MIN:khi - t5.GK_MIN + 1]
            kbest = klo + int(np.argmax(seg))
            snr, margin = features(mags, klo, khi, kbest, mag, est)
            snrs.append(snr)
            margins.append(margin)
            if e >= 2.0:
                wrong.append("%s/%d->%.1f" % (style, bpm, est))
            if verbose:
                print("   %-9s %3d  est %8.2f  err %6.3f  snr %6.2f  margin %6.2f" %
                      (style, bpm, est, e, snr, margin))
    print("%-14s NFFT %5d  wrong %d  mean|err| %6.3f  max %6.3f  "
          "snr>=%.1f  margin>=%.2f  %s" %
          (name, nfft, len(wrong), sum(errs) / len(errs), max(errs),
           min(snrs), min(margins), " ".join(wrong)))
    return len(wrong)


def main():
    from _tempo3_sim import filter_designs
    ds = filter_designs()
    for name in ("hp40_lp160", "hp40_lp160x2", "lp160x2", "bpf85"):
        for nfft in (2048, 4096):
            run(name, ds[name], nfft)
    print()
    print("--- the chosen configuration, per track ---")
    run("hp40_lp160x2", ds["hp40_lp160x2"], 2048, verbose=True)


if __name__ == "__main__":
    main()

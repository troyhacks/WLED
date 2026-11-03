#!/usr/bin/env python3
"""Experiment 8: the CONFIDENCE, measured.

The tempo is published with a confidence, so the confidence has to be
something measurable rather than something hopeful.  Four features are
measured on the same 48 tracks:

  snr_db    20*log10(winner magnitude / median of the in-band background,
           with a +-3 bin guard around the winner)
  margin_db 20*log10(winner / the highest OTHER in-band peak)
  half_ok   1 if the same peak (within 2 BPM) is found again in the
           previous 6 s of the window
  conc      the winner's share of the in-band spectral energy

A feature that does not separate the correct tracks from the wrong one is
not worth publishing.

    python tools/ar_sim/_tempo8_conf.py
"""

import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _tempo5_sim as t5                                            # noqa: E402
from _tempo2_sim import STYLES                                      # noqa: E402

BPMS = (90, 100, 120, 128, 140, 150, 160, 174)


def features(mags, klo, khi, bpm):
    """The confidence features for one window."""
    peaks = t5.local_peaks_strict(mags, klo, khi)
    peaks.sort(key=lambda t: -t[1])
    if not peaks:
        return None
    # the winner: the tallest peak, refined
    kb = t5.peak_strict(mags, klo, khi)
    if kb is None or kb[0] is None:
        return None
    bpm_w, mag_w = kb
    seg = mags[klo - t5.GK_MIN:khi - t5.GK_MIN + 1]
    kbest = klo + int(np.argmax(seg))
    guard = [k - t5.GK_MIN for k in range(klo, khi + 1) if abs(k - kbest) > 3]
    bg = float(np.median(mags[guard])) if guard else 0.0
    snr = 20.0 * math.log10((mag_w + 1e-30) / (bg + 1e-30))
    other = [m for b, m in peaks if abs(b - bpm_w) > 2.0]
    margin = (20.0 * math.log10((mag_w + 1e-30) / (other[0] + 1e-30))
              if other else 40.0)
    tot = float(np.sum(mags[klo:khi + 1])) + 1e-30
    conc = mag_w / tot
    return bpm_w, snr, margin, conc


def half_agree(odf, klo, khi):
    """The same peak found again in the previous half of the window."""
    half = len(odf) // 2
    odf2 = odf[len(odf) - half:]
    if len(odf2) < 32:
        return 0
    mags2 = t5.running_dft_grid(odf2)
    kb = t5.peak_strict(mags2, klo, khi)
    return kb


def main():
    lo, hi = 90.0, 175.0
    klo, khi = t5.band_bins(lo, hi)
    print("%-9s %5s %9s %8s %9s %7s %7s %6s" %
          ("style", "bpm", "est", "snr_db", "margin_db", "conc", "half", "ok"))
    rows = []
    for style in STYLES:
        for bpm in BPMS:
            odf, mags, cand = t5.build(style, bpm)
            f = features(mags, klo, khi, bpm)
            if f is None:
                print("%-9s %5d  NO PEAK" % (style, bpm))
                continue
            est, snr, margin, conc = f
            kb2 = half_agree(odf, klo, khi)
            hok = 0
            if kb2 is not None:
                hok = 1 if abs(kb2[0] - est) < 2.0 else 0
            err = abs(est - bpm)
            ok = err < 2.0
            rows.append((style, bpm, est, snr, margin, conc, hok, ok))
            print("%-9s %5d %9.2f %8.2f %9.2f %7.4f %7d %6s" %
                  (style, bpm, est, snr, margin, conc, hok,
                   "ok" if ok else "WRONG"))
    good = [r for r in rows if r[7]]
    bad = [r for r in rows if not r[7]]
    print()
    for name, idx in (("snr_db", 3), ("margin_db", 4), ("conc", 5)):
        if good:
            v = [r[idx] for r in good]
            print("%-10s correct: min %8.3f  median %8.3f  max %8.3f" %
                  (name, min(v), float(np.median(v)), max(v)))
        if bad:
            v = [r[idx] for r in bad]
            print("%-10s WRONG  : min %8.3f  median %8.3f  max %8.3f" %
                  (name, min(v), float(np.median(v)), max(v)))
    print("half_ok agrees on %d of %d correct tracks" %
          (sum(1 for r in good if r[6]), len(good)))


if __name__ == "__main__":
    main()

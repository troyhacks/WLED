#!/usr/bin/env python3
"""Experiment 11: the SHIPPED architecture, end to end, with the final rules.

  1. the front end (hp40_lp160x2) -> the ODF at 93.75 Hz
  2. the running DFT -> the strict band 90-175 -> the peak
  3. the 3:2 tie-break (prefer the dotted-quarter partner within 3 dB)
  4. the DP over the onset candidates, AT THAT ONE PERIOD -> the beat grid
  5. the least-squares fit of the grid -> the grid's own period (published,
     but NOT used as the BPM: measured worse than the spectral peak)

Everything the firmware would publish, per track.

    python tools/ar_sim/_tempo11_final.py
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


def mags_for(style, bpm):
    key = (style, bpm)
    if key not in CACHE:
        x = t5.get_track(style, bpm, 0.0, 0.0)
        from _tempo3_sim import filter_designs
        odf = t5.odf_fast(x, filter_designs()["hp40_lp160x2"])
        CACHE[key] = (odf, t5.running_dft_grid(odf))
    return CACHE[key]


def main():
    lo, hi = 90.0, 175.0
    klo, khi = t5.band_bins(lo, hi)
    print("%-9s %5s %9s %9s %8s %8s %7s %8s" %
          ("style", "bpm", "spectral", "grid", "err", "F1", "beats", "trans"))
    per_style = {}
    wrong = 0
    errs = []
    f1s = []
    tot_tr = 0
    n = 0
    for style in STYLES:
        for bpm in BPMS:
            odf, mags = mags_for(style, bpm)
            spec, mag = t5.peak_strict(mags, klo, khi)
            spec = t5.rational_slower(spec, mag, mags, klo, khi, 3.0)
            odn = odf / max(1e-30, float(np.max(odf)))
            cand = t5.onset_candidates(odf)
            prop = [60.0 * t5.RATE / spec] if spec else []
            p, beats, tr = t5.dp_pick(cand, odn, prop)
            tot_tr += tr
            grid_bpm = 0.0
            if len(beats) >= 3:
                bp = t5.beat_period(beats)
                if bp > 0:
                    grid_bpm = 60.0 * t5.RATE / bp
            tb = t5.true_beats(bpm, 16.0)
            f1 = t5.f1_score(beats, tb, t5.F1_TOL * t5.RATE)
            e = abs(spec - bpm)
            errs.append(e)
            f1s.append(f1)
            per_style.setdefault(style, []).append((e, f1))
            if e >= 2.0:
                wrong += 1
            n += 1
            print("%-9s %5d %9.2f %9.2f %8.3f %8.3f %7d %8d" %
                  (style, bpm, spec, grid_bpm, e, f1, len(beats), tr))
    print()
    print("spectral wrong %d of %d   mean |err| %.3f BPM" %
          (wrong, n, sum(errs) / n))
    print("mean grid F1 %.3f   mean DP transitions %.0f" %
          (sum(f1s) / n, tot_tr / n))
    print()
    print("%-9s %8s %8s" % ("style", "meanerr", "meanF1"))
    for style, rows in per_style.items():
        print("%-9s %8.3f %8.3f" %
              (style, sum(r[0] for r in rows) / len(rows),
               sum(r[1] for r in rows) / len(rows)))


if __name__ == "__main__":
    main()

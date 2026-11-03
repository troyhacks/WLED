#!/usr/bin/env python3
"""The SHIPPED architecture, measured: spectrum decides the period, the DP
lays the beat grid on it, the least-squares fit of the grid refines the BPM.

    python tools/ar_sim/_tempo6_arch.py

This is the design the firmware will implement, so this is the number that
matters:
  * the BPM comes from the SPECTRUM's peak (the strict band, the rational
    tie-break) - and then from the DP's beat grid, refined by a
    least-squares line through the beats
  * the DP runs at ONE period: the spectral winner's.  Not at six proposed
    periods, not at a 50-period grid.
  * the beat F1 says whether the grid is right
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _tempo5_sim as t5                                            # noqa: E402
from _tempo2_sim import STYLES                                      # noqa: E402


def run(band_lo=90.0, band_hi=175.0, tie_db=3.0, dp_at="winner"):
    bpms = (90, 100, 120, 128, 140, 150, 160, 174)
    print("band %.0f-%.0f  tie %.1f dB  DP at: %s" %
          (band_lo, band_hi, tie_db, dp_at))
    print("%-9s %5s %9s %9s %8s %8s %8s %8s" %
          ("style", "bpm", "spec", "final", "err", "F1", "cands", "trans"))
    wrong = 0
    wrong_spec = 0
    f1s = []
    errs = []
    trans_tot = 0
    n = 0
    for style in STYLES:
        for bpm in bpms:
            odf, mags, cand = t5.build(style, bpm)
            odn = odf / max(1e-30, float(np.max(odf)))
            klo, khi = t5.band_bins(band_lo, band_hi)
            peaks = t5.local_peaks_strict(mags, klo, khi)
            peaks.sort(key=lambda t: -t[1])
            spec, mag, _ = t5.spectral_pick(mags, band_lo, band_hi, tie_db)
            if dp_at == "winner":
                props = [60.0 * t5.RATE / spec] if spec else []
            elif dp_at == "top3":
                props = [60.0 * t5.RATE / b for b, _ in peaks[:3] if b > 0]
            else:
                props = [60.0 * t5.RATE / b for b, _ in peaks[:6] if b > 0]
            p, beats, tr = t5.dp_pick(cand, odn, props)
            trans_tot += tr
            final = spec
            if len(beats) >= 3:
                bp = t5.beat_period(beats)
                if bp > 0.0:
                    final = 60.0 * t5.RATE / bp
            tb = t5.true_beats(bpm, 16.0)
            f1 = t5.f1_score(beats, tb, t5.F1_TOL * t5.RATE)
            f1s.append(f1)
            e = abs(final - bpm)
            errs.append(e)
            if e >= 2.0:
                wrong += 1
            if abs(spec - bpm) >= 2.0:
                wrong_spec += 1
            n += 1
            print("%-9s %5d %9.2f %9.2f %8.2f %8.3f %8d %8d" %
                  (style, bpm, spec, final, e, f1, len(cand), tr))
    print()
    print("spectral wrong %d of %d   final wrong %d of %d" %
          (wrong_spec, n, wrong, n))
    print("mean |err| %.2f BPM   mean F1 %.3f   mean transitions %.0f"
          % (sum(errs) / n, sum(f1s) / n, trans_tot / n))
    return wrong, wrong_spec


if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv) > 1 else "winner"
    run(dp_at=which)

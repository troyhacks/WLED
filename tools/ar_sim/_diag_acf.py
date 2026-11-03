#!/usr/bin/env python3
"""Where does the autocorrelation estimator go wrong, and is librosa a
trustworthy oracle?

Two questions, because they decide whether the numbers mean anything:

1. STABILITY.  Each track contributes three windows from its steady part.
   An estimator that is right gives the same answer in all three.  An
   estimator that is wrong gives the same WRONG answer in all three.  So
   stability alone does not prove correctness - but a comparison of the
   estimator's stability against librosa's own stability says which one is
   bouncing around.

2. THE METRICAL LEVEL.  librosa's beat tracker picks a level too, and it
   does not always pick the same one.  If librosa is at the eighth-note
   level in one window and the quarter-note level in the next, then
   librosa's BPM is not a fixed truth and the alignment score against it
   is measuring the oracle's inconsistency as much as the estimator's
   error.

The answer decides whether to keep chasing the estimator or to fix the
oracle.

    python tools/ar_sim/_diag_acf.py
"""

import argparse
import os
import sys
from collections import defaultdict

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _oracle_librosa                             # noqa: E402
import _tempo_acf as T                             # noqa: E402
from _score_acf import grid_alignment              # noqa: E402


def track_of(key):
    return key.split("@")[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--design", default="hp40_lp160x2")
    ap.add_argument("--whiten", type=float, default=0.0)
    ap.add_argument("--window", type=float, default=T.WINDOW_S)
    ap.add_argument("--show", type=int, default=0)
    args = ap.parse_args()

    win = int(args.window * T.RATE)
    per_track = defaultdict(list)

    for k in _cache.keys():
        o = _oracle_librosa.load(k)
        x = _cache.load(k)
        if o is None or x is None:
            continue
        odf = T.odf_cached(k, x, args.design, args.whiten)
        bpm, conf, mag = T.estimate_acf(odf, win=win)
        if bpm is None:
            continue
        f, d, _n = grid_alignment(bpm, o["beats"])
        # librosa's OWN tempo from its beat times, which is more consistent
        # than its reported BPM
        bt = np.asarray(o["beats"], dtype=float)
        ibi = np.diff(bt)
        lib_ibi = float(np.median(ibi)) if len(ibi) else 0.0
        lib_bpm_ibi = 60.0 / lib_ibi if lib_ibi > 0 else 0.0
        per_track[track_of(k)].append(
            dict(key=k, acf=bpm, conf=conf, lib=o["bpm"], lib_ibi=lib_bpm_ibi,
                 align=f, dev=d, nbeats=len(bt)))

    print("%d tracks, %d windows" % (len(per_track),
                                     sum(len(v) for v in per_track.values())))

    # --- stability ------------------------------------------------------
    def spread(vals):
        if len(vals) < 2:
            return 0.0
        return float(max(vals) - min(vals))

    acf_sp = []
    lib_sp = []
    libibi_sp = []
    for t, rows in per_track.items():
        acf_sp.append(spread([r["acf"] for r in rows]))
        lib_sp.append(spread([r["lib"] for r in rows]))
        libibi_sp.append(spread([r["lib_ibi"] for r in rows]))

    for name, sp in (("acf", acf_sp), ("librosa bpm", lib_sp),
                     ("librosa from beats", libibi_sp)):
        sp = np.array(sp)
        print("%-20s spread across the 3 windows of a track:  median %6.2f   "
              "<=2 BPM: %5.1f%%   <=5: %5.1f%%   >20: %5.1f%%"
              % (name, float(np.median(sp)),
                 100.0 * float((sp <= 2).mean()), 100.0 * float((sp <= 5).mean()),
                 100.0 * float((sp > 20).mean())))

    # --- agreement ------------------------------------------------------
    # compare the ACF estimate against librosa's tempo DERIVED FROM ITS
    # OWN BEATS, which is the more stable of the two librosa numbers
    diffs = []
    rat = []
    for t, rows in per_track.items():
        for r in rows:
            if r["lib_ibi"] > 0:
                diffs.append(r["acf"] - r["lib_ibi"])
                rat.append(r["acf"] / r["lib_ibi"])
    diffs = np.array(diffs)
    rat = np.array(rat)
    print()
    print("acf vs librosa's beat-derived tempo:  median %+6.2f BPM   "
          "ratio median %.4f   ratio in [0.98,1.02]: %.1f%%   "
          "ratio in [0.49,0.51] or [1.98,2.02]: %.1f%%"
          % (float(np.median(diffs)), float(np.median(rat)),
             100.0 * float(((rat > 0.98) & (rat < 1.02)).mean()),
             100.0 * float((((rat > 0.49) & (rat < 0.51)) |
                            ((rat > 1.98) & (rat < 2.02))).mean())))

    # the histogram of the ratio - it should have ONE tall peak at 1.0
    hist, edges = np.histogram(rat, bins=40, range=(0.3, 2.2))
    print()
    print("the ratio's histogram (one tall peak at 1.0 = agreement;")
    print("a peak at 0.5 = the estimator is an octave off):")
    for h, lo, hi in zip(hist, edges[:-1], edges[1:]):
        if h:
            print("  %5.2f-%5.2f  %s %d" % (lo, hi, "#" * h, h))

    # --- the failures ---------------------------------------------------
    print()
    bad = [(r["acf"], r["lib_ibi"], r["align"], r["conf"], r["key"])
           for rows in per_track.values() for r in rows
           if r["align"] < 0.5]
    print("%d of %d windows align below 0.5" % (len(bad), sum(len(v) for v in per_track.values())))
    bad.sort(key=lambda x: x[3])            # lowest confidence first
    for a, lb, al, cf, k in bad[:args.show if args.show else 12]:
        print("  %-44s acf %7.2f  lib %7.2f  align %4.2f  conf %6.2f"
              % (k[:44], a, lb, al, cf))

    # is confidence predictive of correctness?
    print()
    confs = np.array([r["conf"] for rows in per_track.values() for r in rows])
    aligns = np.array([r["align"] for rows in per_track.values() for r in rows])
    for lo, hi in ((0, 1), (1, 2), (2, 5), (5, 10), (10, 1e9)):
        m = (confs >= lo) & (confs < hi)
        if m.sum():
            print("  conf %5.1f-%6.1f : %3d windows   mean align %5.3f   "
                  "frac align>=0.5 %5.1f%%"
                  % (lo, hi, int(m.sum()), float(aligns[m].mean()),
                     100.0 * float((aligns[m] >= 0.5).mean())))


if __name__ == "__main__":
    sys.exit(main())

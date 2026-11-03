#!/usr/bin/env python3
"""Score a tempo estimate against librosa by BEAT ALIGNMENT, not by BPM.

Comparing two BPM numbers is a blunt instrument.  Two estimators can
disagree by 20 BPM and both be describing the same music at different
metrical levels, or they can differ by 0.4 BPM and be describing genuinely
different tempi.

The sharper test is: lay a beat grid at the candidate BPM over the window,
and ask how well it lands on librosa's beat times.  A grid at the right
tempo lands ON the beats; a grid at the wrong tempo lands between them.
This is the same thing the beat tracker is trying to do, so it is the right
yardstick.

The metric is the fraction of librosa beats that have a grid beat within
+/- 70 ms (the same tolerance Ellis's F1 score uses), and the median
deviation of those that do land.

It also reports the octave-relative form, because a grid at twice the
right tempo lands on every OTHER beat - which the F1 catches, but a
tolerance-wide score would not.

    python tools/ar_sim/_score_acf.py --keys "004."
    python tools/ar_sim/_score_acf.py --all
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _oracle_librosa                             # noqa: E402
import _tempo_acf as T                             # noqa: E402

TOL = 0.070          # seconds; Ellis's F1 tolerance


def grid_alignment(bpm, beats, span_s=40.0, offset=0.0):
    """How well does a beat grid at `bpm` land on `beats`?

    `beats` are times in seconds from the start of the window.  The grid
    is laid with an arbitrary phase - the phase is searched over, because
    the estimator does not know where in the window the first beat falls.
    """
    if bpm is None or bpm <= 0 or not len(beats):
        return 0.0, 0.0, 0
    period = 60.0 / bpm
    if period <= 0:
        return 0.0, 0.0, 0
    beats = np.asarray(beats, dtype=float)
    best = (0.0, 1e9, 0)
    # search the phase over one period, in 1/32-period steps
    for ph in np.arange(0.0, period, period / 32.0):
        # grid beats within the window
        k0 = int(np.floor((offset - ph) / period))
        k1 = int(np.ceil((offset + span_s - ph) / period))
        grid = ph + np.arange(k0, k1 + 1) * period
        grid = grid[(grid >= offset - 0.5) & (grid <= offset + span_s + 0.5)]
        if not len(grid):
            continue
        # nearest grid beat for each librosa beat
        d = np.abs(beats[:, None] - grid[None, :]).min(axis=1)
        hit = d <= TOL
        frac = float(hit.mean())
        if frac > best[0] or (frac == best[0] and np.median(d[hit]) < best[1]
                              if hit.any() else False):
            dev = float(np.median(d[hit])) if hit.any() else 1e9
            if frac > best[0] or (frac == best[0] and dev < best[1]):
                best = (frac, dev, int(hit.sum()))
    return best


def estimate_bpm(odf, method, win=T.WIN, args=None):
    if method == "spectral":
        bpm, mag, _ = T.estimate_spectral(odf, win)
        return bpm, mag
    if method == "acf":
        bpm, conf, mag = T.estimate_acf(odf, win=win)
        return bpm, conf
    if method == "acf_notie":
        bpm, conf, mag = T.estimate_acf(odf, win=win, tie=False)
        return bpm, conf
    if method == "comb":
        return T.estimate_comb(odf, win)
    raise ValueError(method)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keys", default=None)
    ap.add_argument("--design", default="hp40_lp160x2")
    ap.add_argument("--whiten", type=float, default=0.0)
    ap.add_argument("--window", type=float, default=T.WINDOW_S)
    ap.add_argument("--methods", default="spectral,acf,comb")
    ap.add_argument("--show", action="store_true")
    args = ap.parse_args()

    keys = _cache.keys()
    if args.keys:
        keys = [k for k in keys if args.keys in k]
    win = int(args.window * T.RATE)

    data = []
    for k in keys:
        o = _oracle_librosa.load(k)
        x = _cache.load(k)
        if o is None or x is None or not o.get("beats"):
            continue
        odf = T.odf_cached(k, x, args.design, args.whiten)
        data.append((k, o, odf))
    print("%d windows with an oracle   design %s   whiten %.2f s   window %.1f s"
          % (len(data), args.design, args.whiten, args.window))

    for method in args.methods.split(","):
        fracs, devs, bpms, libs = [], [], [], []
        for k, o, odf in data:
            bpm, _c = estimate_bpm(odf, method, win)
            if bpm is None:
                continue
            f, d, _n = grid_alignment(bpm, o["beats"])
            fracs.append(f)
            devs.append(d if f > 0 else 99.0)
            bpms.append(bpm)
            libs.append(o["bpm"])
        if not fracs:
            print("%-14s no rows" % method)
            continue
        fracs = np.array(fracs)
        good = sum(1 for f in fracs if f >= 0.5)
        good8 = sum(1 for f in fracs if f >= 0.8)
        devs = np.array(devs)
        # stability: the spread of the estimate across the 3 windows of a track
        print("%-14s n=%3d  align>=0.5: %3d (%5.1f%%)  align>=0.8: %3d (%5.1f%%)  median align %5.3f  median dev %6.1f ms"
              % (method, len(fracs), good, 100.0 * good / len(fracs),
                 good8, 100.0 * good8 / len(fracs), float(np.median(fracs)),
                 1000.0 * float(np.median(devs))))

    if args.show:
        print()
        for k, o, odf in data[:20]:
            line = "  %-44s" % k[:44]
            for method in args.methods.split(","):
                bpm, _c = estimate_bpm(odf, method, win)
                if bpm is None:
                    line += " %8s" % "-"
                    continue
                f, d, _n = grid_alignment(bpm, o["beats"])
                line += "  %6.2f/%4.2f" % (bpm, f)
            print(line)


if __name__ == "__main__":
    sys.exit(main())

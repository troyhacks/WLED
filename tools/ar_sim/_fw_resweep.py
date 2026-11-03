#!/usr/bin/env python3
"""RE-CHECK THE CONSTANTS WITH THE EXACT KERNEL.

The comb's kernel changed - the half-width is no longer rounded to whole
bins - so the scores moved a little, and every constant that was chosen
against those scores has to be chosen again:

  BAND     the band the tracker listens in
  MARGIN   how much better a far-away rate must be before it may move
            the grid

The PLL's gain is swept separately, by _fw_pll.py, because it needs the
re-lock measurement as well and that takes much longer.

    python tools/ar_sim/_fw_resweep.py --windows 40
"""

import argparse
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402

BANDS = [
    (80.0, 320.0),
    (90.0, 320.0),
    (100.0, 320.0),
    (100.0, 400.0),
    (80.0, 400.0),
    (60.0, 320.0),
]

MARGINS = [0.0, 0.10, 0.20, 0.30, 0.45]


def make(margin=0.20, gain=0.10):
    t = P.Tracker()
    t.narrow_bpm = 2.0
    t.search_every = 4
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = 32
    t.net_margin = margin
    t.pll_gain = gain
    return t


def wander(t):
    log = np.array(t.bpm_log)
    half = log[len(log) // 2:]
    half = half[half > 0]
    if len(half) < 50:
        return 0.0, 0, 0.0
    jumps = int(np.sum(np.abs(np.diff(half)) > 0.5))
    biggest = float(np.max(np.abs(np.diff(half)))) if len(half) > 1 else 0.0
    return float(np.std(half)), jumps, biggest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=40)
    ap.add_argument("--band-lo", type=float, default=80.0)
    ap.add_argument("--band-hi", type=float, default=320.0)
    ap.add_argument("--skip-band", action="store_true",
                    help="only the margin, at --band-lo/--band-hi")
    ap.add_argument("--margins", type=str, default=None,
                    help="comma-separated margins, e.g. 0.35,0.45,0.55")
    args = ap.parse_args()
    margins = ([float(v) for v in args.margins.split(",")]
               if args.margins else MARGINS)

    t0 = time.time()
    items = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        if len(ons) < 4:
            continue
        wons = B.wide_onsets(x)
        if len(wons) < 4:
            continue
        items.append((k, x, ons, wons))
    print("%d windows   %.0f s" % (len(items), time.time() - t0), flush=True)
    print()

    # ---- the band ------------------------------------------------------
    print("THE BAND  (the tracker listens in it, and the onset")
    print("detector's ground truth does NOT)")
    # TWO ground truths.  The kick onsets are the 40-160 Hz band, and the
    # wide onsets are a 20-2000 Hz detector that catches the snare and the
    # hats as well.  The wide one is the FAIR test: it is not the band the
    # tracker listens in, so a band that only matched itself would not win
    # there.
    print("%-14s %8s %8s | %8s %8s | %8s %8s %8s %8s"
          % ("band", "kick med", "kick mean", "wide med", "wide mean",
             "wander", "jumps", "biggest", "far"))
    for (lo, hi) in ([] if args.skip_band else BANDS):
        khits, whits = [], []
        wand, jumps, bigs, fars = [], [], [], []
        for (k, x, ons, wons) in items:
            odf = B.firmware_odf(x, lo, hi)
            if len(odf) < 1200:
                continue
            t = make()
            t.run(odf)
            if not t.beats:
                khits.append(0.0)
                whits.append(0.0)
                continue
            beats = np.array(t.beats) / P.RATE
            khits.append(P.hit_with_offset(beats, ons)[0])
            whits.append(P.hit_with_offset(beats, wons)[0])
            w, j, b = wander(t)
            wand.append(w)
            jumps.append(j)
            bigs.append(b)
            fars.append(t.net_far)
        ka = np.array(khits)
        wa = np.array(whits)
        print("%-14s %8.4f %8.4f | %8.4f %8.4f | %8.3f %8.1f %8.3f %8.1f"
              % ("%g-%g Hz" % (lo, hi), float(np.median(ka)), float(ka.mean()),
                 float(np.median(wa)), float(wa.mean()),
                 float(np.mean(wand)), float(np.mean(jumps)),
                 float(np.mean(bigs)), float(np.mean(fars))), flush=True)
    print()

    # ---- the margin ----------------------------------------------------
    print("THE MARGIN  (how much better a far rate must be to move the grid)")
    print("%-14s %8s %8s %8s %8s %8s %8s %8s"
          % ("margin", "hit med", "hit mean", "wander", "jumps", "biggest",
             "far", "vetoes"))
    for m in margins:
        hits, wand, jumps, bigs, fars, vet = [], [], [], [], [], []
        for (k, x, ons, wons) in items:
            odf = B.firmware_odf(x, args.band_lo, args.band_hi)
            if len(odf) < 1200:
                continue
            t = make(margin=m)
            t.run(odf)
            if not t.beats:
                hits.append(0.0)
                continue
            f, _off = P.hit_with_offset(np.array(t.beats) / P.RATE, ons)
            hits.append(f)
            w, j, b = wander(t)
            wand.append(w)
            jumps.append(j)
            bigs.append(b)
            fars.append(t.net_far)
            vet.append(t.net_vetoes)
        a = np.array(hits)
        print("%-14.2f %8.4f %8.4f %8.3f %8.1f %8.3f %8.1f %8.1f"
              % (m, float(np.median(a)), float(a.mean()),
                 float(np.mean(wand)), float(np.mean(jumps)),
                 float(np.mean(bigs)), float(np.mean(fars)),
                 float(np.mean(vet))), flush=True)

    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

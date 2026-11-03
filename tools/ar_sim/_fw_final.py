#!/usr/bin/env python3
"""THE LAST TABLE: the band, the margin and the gain, together.

Everything that is left to decide is in here, and it is measured against
TWO ground truths at once:

  kick   the 40-160 Hz onset detector - the beat a DJ counts
  wide   the 20-2000 Hz onset detector - the snare and the hats too, and
         NOT the band the tracker listens in, so it is the fair test

and the tracker is measured three ways:

  hit     the fraction of the ground truth's onsets with a beat near them
  wander  the standard deviation of the published rate
  jumps   how many times the published rate moved by more than half a BPM

    python tools/ar_sim/_fw_final.py --windows 34
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


def make(margin=0.70, gain=0.10, prior=0.6, narrow_prior=True):
    t = P.Tracker()
    t.narrow_bpm = 2.0
    t.search_every = 4
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = 32
    t.net_margin = margin
    t.pll_gain = gain
    t.prior_weight = prior
    t.prior = t._prior_table()
    t.prior_in_narrow = narrow_prior
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
    ap.add_argument("--windows", type=int, default=34)
    ap.add_argument("--bands", type=str,
                    default="80:320,60:320,80:400,90:320")
    ap.add_argument("--margins", type=str, default="0.55,0.70,0.85")
    ap.add_argument("--gains", type=str, default="0.05,0.10,0.20,0.35")
    ap.add_argument("--prior", type=float, default=0.6)
    ap.add_argument("--narrow-prior", type=int, default=1)
    args = ap.parse_args()
    bands = []
    for b in args.bands.split(","):
        lo, hi = b.split(":")
        bands.append((float(lo), float(hi)))
    margins = [float(v) for v in args.margins.split(",")]
    gains = [float(v) for v in args.gains.split(",")]

    t0 = time.time()
    items = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        wons = B.wide_onsets(x)
        if len(ons) < 4 or len(wons) < 4:
            continue
        items.append((k, x, ons, wons))
    print("%d windows   %.0f s" % (len(items), time.time() - t0), flush=True)
    print()

    # ---- the band x the margin ------------------------------------------
    print("THE BAND x THE MARGIN")
    print("%-14s %-7s | %8s %8s | %8s %8s | %7s %6s %6s"
          % ("band", "margin", "kick med", "kick mean", "wide med",
             "wide mean", "wander", "jumps", "far"))
    odfs = {}
    for (lo, hi) in bands:
        key = "%g-%g" % (lo, hi)
        odfs[key] = []
        for (k, x, ons, wons) in items:
            odf = B.firmware_odf(x, lo, hi)
            odfs[key].append((odf, ons, wons))
    for (lo, hi) in bands:
        key = "%g-%g" % (lo, hi)
        for m in margins:
            kh, wh, wd, jp, fa = [], [], [], [], []
            for (odf, ons, wons) in odfs[key]:
                if len(odf) < 1200:
                    continue
                t = make(margin=m, gain=gains[0], prior=args.prior,
                         narrow_prior=bool(args.narrow_prior))
                t.run(odf)
                if not t.beats:
                    kh.append(0.0)
                    wh.append(0.0)
                    continue
                beats = np.array(t.beats) / P.RATE
                kh.append(P.hit_with_offset(beats, ons)[0])
                wh.append(P.hit_with_offset(beats, wons)[0])
                w, j, b = wander(t)
                wd.append(w)
                jp.append(j)
                fa.append(t.net_far)
            ka, wa = np.array(kh), np.array(wh)
            print("%-14s %-7.2f | %8.4f %8.4f | %8.4f %8.4f | %7.3f %6.1f %6.1f"
                  % ("%s Hz" % key, m, float(np.median(ka)), float(ka.mean()),
                     float(np.median(wa)), float(wa.mean()),
                     float(np.mean(wd)), float(np.mean(jp)),
                     float(np.mean(fa))), flush=True)
    print()

    # ---- the gain ---------------------------------------------------------
    print("THE GAIN  (at the first band and the first margin)")
    lo, hi = bands[0]
    key = "%g-%g" % (lo, hi)
    print("%-7s | %8s %8s | %8s %8s | %7s %6s %6s"
          % ("gain", "kick med", "kick mean", "wide med", "wide mean",
             "wander", "jumps", "far"))
    for g in gains:
        kh, wh, wd, jp, fa = [], [], [], [], []
        for (odf, ons, wons) in odfs[key]:
            if len(odf) < 1200:
                continue
            t = make(margin=margins[0], gain=g, prior=args.prior,
                     narrow_prior=bool(args.narrow_prior))
            t.run(odf)
            if not t.beats:
                kh.append(0.0)
                wh.append(0.0)
                continue
            beats = np.array(t.beats) / P.RATE
            kh.append(P.hit_with_offset(beats, ons)[0])
            wh.append(P.hit_with_offset(beats, wons)[0])
            w, j, b = wander(t)
            wd.append(w)
            jp.append(j)
            fa.append(t.net_far)
        ka, wa = np.array(kh), np.array(wh)
        print("%-7.2f | %8.4f %8.4f | %8.4f %8.4f | %7.3f %6.1f %6.1f"
              % (g, float(np.median(ka)), float(ka.mean()),
                 float(np.median(wa)), float(wa.mean()),
                 float(np.mean(wd)), float(np.mean(jp)),
                 float(np.mean(fa))), flush=True)
    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

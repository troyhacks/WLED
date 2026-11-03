#!/usr/bin/env python3
"""THE CREEP OR THE JUMP?

The far candidate won the veto and the grid was WALKED toward it - ten per
cent of the way on every adopt.  From 130 towards 173 that walk passes
through 140 and 150, where nothing aligns at all, and the tracker sat in
that dead zone for seconds, dithering, while its published score was
rubbish.

This measures the two ways of taking a far win:

  CREEP  the proportional PLL, as it is today
  JUMP  the grid is put ON the new rate, and the beat grid is re-anchored
        on the new phase

over a range of margins, on three numbers:

  hit     the fraction of the ground-truth onsets that have a beat near
          them
  wander  the standard deviation of the published rate over the second
          half of the window
  jumps   how many times the rate moved by more than half a BPM

    python tools/ar_sim/_fw_hard.py --windows 58 --margins 0.20,0.45
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


def make(margin=0.20, gain=0.10, hard=False):
    t = P.Tracker()
    t.narrow_bpm = 2.0
    t.search_every = 4
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = 32
    t.net_margin = margin
    t.pll_gain = gain
    t.hard_far = hard
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
    ap.add_argument("--windows", type=int, default=58)
    ap.add_argument("--band-lo", type=float, default=80.0)
    ap.add_argument("--band-hi", type=float, default=320.0)
    ap.add_argument("--margins", type=str, default="0.20,0.45")
    ap.add_argument("--gain", type=float, default=0.10)
    args = ap.parse_args()
    margins = [float(v) for v in args.margins.split(",")]

    t0 = time.time()
    items = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        if len(ons) < 4:
            continue
        odf = B.firmware_odf(x, args.band_lo, args.band_hi)
        if len(odf) < 1200:
            continue
        items.append((k, odf, ons))
    print("%d windows   %.0f s" % (len(items), time.time() - t0), flush=True)
    print()

    print("%-8s %-6s %8s %8s | %8s %8s %8s %8s"
          % ("margin", "way", "hit med", "hit mean", "wander", "jumps",
             "biggest", "far"))
    for m in margins:
        for hard in (False, True):
            hits, wand, jumps, bigs, fars = [], [], [], [], []
            for (k, odf, ons) in items:
                t = make(margin=m, gain=args.gain, hard=hard)
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
            a = np.array(hits)
            print("%-8.2f %-6s %8.4f %8.4f | %8.3f %8.1f %8.3f %8.1f"
                  % (m, "JUMP" if hard else "creep",
                     float(np.median(a)), float(a.mean()),
                     float(np.mean(wand)), float(np.mean(jumps)),
                     float(np.mean(bigs)), float(np.mean(fars))), flush=True)
    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

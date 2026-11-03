#!/usr/bin/env python3
"""REVIEW CHECK: which window wanders, and under which band.

Per window: the hit rate, the settled wander, the biggest single move, how
many times the veto let the FAR rate through, and the range the published
BPM covered.

    python -u tools/ar_sim/_rev_diag.py --windows 12
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


def shipped(gain, margin=0.20):
    t = P.Tracker()
    t.narrow_bpm = 2.0
    t.search_every = 4
    t.full_every = 32
    t.collapse_trigger = True
    t.edge_rule = False
    t.net_margin = margin
    t.pll_gain = gain
    return t


def wander(t):
    log = np.array(t.bpm_log)
    half = log[len(log) // 2:]
    half = half[half > 0]
    if len(half) < 100:
        return 0.0, 0, 0.0, 0.0, 0.0
    jumps = int(np.sum(np.abs(np.diff(half)) > 0.5))
    biggest = float(np.max(np.abs(np.diff(half)))) if len(half) > 1 else 0.0
    return (float(np.std(half)), jumps, biggest,
            float(half.min()), float(half.max()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=12)
    args = ap.parse_args()

    t0 = time.time()
    items = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        if len(ons) < 4:
            continue
        items.append((k, x, ons))
    print("%d windows  %.0f s" % (len(items), time.time() - t0), flush=True)

    for (lo, hi) in ((80.0, 320.0), (40.0, 160.0)):
        print()
        print("BAND %g-%g Hz" % (lo, hi))
        print("%-34s %-5s %6s %7s %7s %7s %7s %7s %5s %5s"
              % ("window", "gain", "hit", "wander", "jumps", "biggest",
                 "bpmmin", "bpmmax", "far", "veto"))
        for (k, x, ons) in items:
            odf = B.firmware_odf(x, lo, hi)
            if len(odf) < 1200:
                continue
            for gain in (0.0, 0.10):
                t = shipped(gain)
                t.run(odf)
                if not t.beats:
                    print("%-34s %-5.2f   (no beats)" % (k[:34], gain),
                          flush=True)
                    continue
                f, _off = P.hit_with_offset(np.array(t.beats) / P.RATE, ons)
                w, j, b, mn, mx = wander(t)
                print("%-34s %-5.2f %6.3f %7.3f %7.1f %7.3f %7.1f %7.1f %5d %5d"
                      % (k[:34], gain, f, w, j, b, mn, mx,
                         t.net_far, t.net_vetoes), flush=True)
    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

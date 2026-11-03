#!/usr/bin/env python3
"""THE PLL'S RATE RESPONSE: A CLAMP, OR A FRACTION?

The clamp lets the period move at most 0.5 % per adopt, so it always
applies the FULL step when the comb's winner is a candidate away - even
when that winner is only a quarter of a BPM away and is about to flip
back.  The rate then ripples.

A proportional response moves a FRACTION of the way to the target, so a
quarter-BPM flip moves the rate by a sixteenth of a BPM, and a real
change is still taken in a handful of adopts.

    python tools/ar_sim/_fw_pll.py --windows 36 --pairs 12
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

GAINS = [0.0, 0.05, 0.10, 0.20, 0.35, 0.50]
MARGIN = 0.20
TOL_BPM = 1.0
HOLD_FRAMES = 281


def make(gain, margin=MARGIN):
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
    if len(half) < 100:
        return 0.0, 0, 0.0
    jumps = int(np.sum(np.abs(np.diff(half)) > 0.5))
    biggest = float(np.max(np.abs(np.diff(half)))) if len(half) > 1 else 0.0
    return float(np.std(half)), jumps, biggest


def reference_bpm(odf):
    t = P.Tracker()
    t.narrow_bpm = 0.0
    t.run(odf)
    return t.bpm if t.locked and t.bpm > 0 else None


def relock_time(t, cut, ref):
    log = t.bpm_log
    n = len(log)
    first = None
    for f in range(cut, n):
        if t.locked and abs(log[f] - ref) <= TOL_BPM:
            first = f
            break
    if first is None:
        return None
    for f in range(first, min(n, first + HOLD_FRAMES)):
        if not t.locked or abs(log[f] - ref) > 2.0 * TOL_BPM:
            return None
    return (first - cut) / P.RATE


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=36)
    ap.add_argument("--pairs", type=int, default=12)
    ap.add_argument("--lo", type=float, default=80.0)
    ap.add_argument("--hi", type=float, default=320.0)
    args = ap.parse_args()

    t0 = time.time()
    items = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, args.lo, args.hi)
        if len(odf) < 1200:
            continue
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        if len(ons) < 4:
            continue
        items.append((k, odf, ons))
    print("%d windows   %.0f s" % (len(items), time.time() - t0), flush=True)

    # The reference rate of every window, ONCE.  Asking for it inside the
    # pairing loop asked for it O(n^2) times, and each ask is a whole-band
    # tracker run - which is why the sweep took an hour to print a second
    # line.
    refs = []
    for k in range(len(items)):
        refs.append(reference_bpm(items[k][1]))
        if (k + 1) % 20 == 0:
            print("   reference %d/%d" % (k + 1, len(items)), flush=True)

    pairs = []
    for i in range(len(items)):
        bpm = refs[i]
        if bpm is None:
            continue
        for j in range(i + 1, len(items)):
            bpm2 = refs[j]
            if bpm2 is None or abs(bpm2 - bpm) < 6.0:
                continue
            pairs.append((items[i], items[j], bpm2))
            break
        if len(pairs) >= args.pairs:
            break
    print("%d pairs   %.0f s" % (len(pairs), time.time() - t0), flush=True)
    print()

    print("%-7s %8s %8s %8s %8s | %10s %8s %8s"
          % ("gain", "hit", "wander", "jumps", "biggest", "relock s", "worst",
             "never"))
    for g in GAINS:
        hits, wand, jumps, bigs = [], [], [], []
        for (k, odf, ons) in items:
            t = make(g)
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
        times, never = [], 0
        for (a, b, ref) in pairs:
            t = make(g)
            cut = len(a[1])
            t.run(np.concatenate((a[1], b[1])))
            r = relock_time(t, cut, ref)
            if r is None:
                never += 1
                times.append(999.0)
            else:
                times.append(r)
        arr = np.array(times)
        print("%-7.2f %8.3f %8.3f %8.1f %8.3f | %10.2f %8.2f %8d"
              % (g, float(np.median(hits)), float(np.mean(wand)),
                 float(np.mean(jumps)), float(np.mean(bigs)),
                 float(np.median(arr)), float(arr.max()), never), flush=True)

    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

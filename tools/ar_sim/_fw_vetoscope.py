#!/usr/bin/env python3
"""SHOULD THE VETO APPLY TO THE COLLAPSE SEARCH TOO?

THE QUESTION
------------
There are TWO whole-band searches, and they mean different things:

  SAFETY NET  the grid is happy and the score is fine; the search runs
              every so often just in case the music changed under the
              grid's feet.  The far rate it finds is a NEAR-TIE most of
              the time, and it drags the grid - that is the 125/131 fight
              the veto was built for.

  COLLAPSE    the narrow search could not find the beat at all: its
              score fell below the minimum.  That is a STATEMENT that
              the old rate is gone, not a near-tie.  The far rate is the
              only evidence left, and if the veto refuses it the grid can
              sit on a dead rate.

So the veto may be right for the safety net and wrong for the collapse.
This measures both, side by side, over the same corpus:

  THE WANDER    how far the rate moves after it has settled.
  THE RE-LOCK   how long the tracker keeps reporting the OLD tempo after
                the music changed, and how often it NEVER re-locks.
  THE HIT RATE  the fraction of onsets that get a beat.

    python tools/ar_sim/_fw_vetoscope.py --windows 36 --pairs 12
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

VARIANTS = [
    ("no veto",        0.00, True),
    ("veto everywhere", 0.20, True),
    ("veto on net only", 0.20, False),
]

NARROW = 2.0
EVERY = 4
NET = 32
TOL_BPM = 1.0
HOLD_FRAMES = 281


def make(margin, veto_on_collapse):
    t = P.Tracker()
    t.narrow_bpm = NARROW
    t.search_every = EVERY
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = NET
    t.net_margin = margin
    t.veto_on_collapse = veto_on_collapse
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

    pairs = []
    for i in range(len(items)):
        bpm = reference_bpm(items[i][1])
        if bpm is None:
            continue
        for j in range(i + 1, len(items)):
            bpm2 = reference_bpm(items[j][1])
            if bpm2 is None or abs(bpm2 - bpm) < 6.0:
                continue
            pairs.append((items[i], items[j], bpm2))
            break
        if len(pairs) >= args.pairs:
            break
    print("%d pairs   %.0f s" % (len(pairs), time.time() - t0), flush=True)
    print()

    print("%-18s %8s %8s %8s %8s %8s %8s"
          % ("variant", "hit", "wander", "jumps", "biggest", "vetoed", "far"))
    for (name, margin, voc) in VARIANTS:
        hits, wand, jumps, bigs, veto, far = [], [], [], [], [], []
        for (k, odf, ons) in items:
            t = make(margin, voc)
            t.run(odf)
            if not t.beats:
                hits.append(0.0)
                wand.append(0.0)
                jumps.append(0)
                bigs.append(0.0)
                veto.append(0.0)
                far.append(0.0)
                continue
            f, _off = P.hit_with_offset(np.array(t.beats) / P.RATE, ons)
            hits.append(f)
            w, j, b = wander(t)
            wand.append(w)
            jumps.append(j)
            bigs.append(b)
            veto.append(t.net_vetoes)
            far.append(t.net_far)
        print("%-18s %8.3f %8.3f %8.1f %8.2f %8.1f %8.1f"
              % (name, float(np.median(hits)), float(np.mean(wand)),
                 float(np.mean(jumps)), float(np.mean(bigs)),
                 float(np.mean(veto)), float(np.mean(far))), flush=True)

    print()
    print("%-18s %10s %8s %8s %10s" % ("variant", "median s", "worst s",
                                       "never", "over 30 s"))
    for (name, margin, voc) in VARIANTS:
        times, never = [], 0
        for (a, b, ref) in pairs:
            t = make(margin, voc)
            cut = len(a[1])
            t.run(np.concatenate((a[1], b[1])))
            r = relock_time(t, cut, ref)
            if r is None:
                never += 1
                times.append(999.0)
            else:
                times.append(r)
        arr = np.array(times)
        print("%-18s %10.2f %8.2f %8d %10d"
              % (name, float(np.median(arr)), float(arr.max()), never,
                 int(np.sum(arr > 30.0))), flush=True)

    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

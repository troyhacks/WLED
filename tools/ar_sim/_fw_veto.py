#!/usr/bin/env python3
"""THE VETO: HOW CLEAR MUST THE EVIDENCE BE?

THE CHANGE
----------
The whole-band safety net used to move the grid whenever it found a
better rate anywhere in the band.  The narrow search then moved it back,
and the board answered 125.00 and then 131.00 and then 125.00 again.

Now the net's far rate may only move the grid if it is CLEARLY better
evidence than the local rate - measured on the raw score, on the same
onset snapshot, with the same tolerance.  The prior is a tie-breaker and
not evidence, so it is left out of the comparison.

This measures the trade the veto makes, over a range of thresholds:

  THE WANDER      how far the rate moves after it has settled.  This is
                  what Troy sees, and it must be small.
  THE RE-LOCK     how long the tracker keeps reporting the OLD tempo after
                  the music has changed.  The veto must not stop this.
  THE HIT RATE    the fraction of onsets that get a beat.  The veto must
                  not cost accuracy.
  THE COST        the candidates scored a frame.

    python tools/ar_sim/_fw_veto.py --windows 36 --pairs 12
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

MARGINS = [0.0, 0.10, 0.15, 0.20, 0.25, 0.30]

NARROW = 2.0
EVERY = 4
NET = 32
TOL_BPM = 1.0
HOLD_FRAMES = 281


def make(margin):
    t = P.Tracker()
    t.narrow_bpm = NARROW
    t.search_every = EVERY
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = NET
    t.net_margin = margin
    return t


def wander(t):
    log = np.array(t.bpm_log)
    half = log[len(log) // 2:]
    half = half[half > 0]
    if len(half) < 100:
        return 0.0, 0, 0.0
    jumps = int(np.sum(np.abs(np.diff(half)) > 0.5))
    # the biggest single move in the settled half
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

    # the pairs, for the re-lock
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

    # ---- the corpus: the wander and the hit rate -------------------------
    print("%-8s %8s %8s %8s %8s %8s %8s %8s"
          % ("margin", "hit", "score", "wander", "jumps", "biggest", "vetoed",
             "cand/frm"))
    for m in MARGINS:
        hits, scores, wand, jumps, bigs, veto, cost = [], [], [], [], [], [], []
        for (k, odf, ons) in items:
            t = make(m)
            t.run(odf)
            if not t.beats:
                hits.append(0.0)
                scores.append(0.0)
                wand.append(0.0)
                jumps.append(0)
                bigs.append(0.0)
                cost.append(0.0)
                continue
            f, _off = P.hit_with_offset(np.array(t.beats) / P.RATE, ons)
            hits.append(f)
            scores.append(t.score)
            w, j, b = wander(t)
            wand.append(w)
            jumps.append(j)
            bigs.append(b)
            veto.append(t.net_vetoes)
            cost.append(t.cand_total / float(max(1, t.frame_index)))
        print("%-8.2f %8.3f %8.3f %8.3f %8.1f %8.2f %8.1f %8.1f"
              % (m, float(np.median(hits)), float(np.median(scores)),
                 float(np.mean(wand)), float(np.mean(jumps)), float(np.mean(bigs)),
                 float(np.mean(veto)), float(np.mean(cost))), flush=True)

    # ---- the re-lock ----------------------------------------------------
    print()
    print("%-8s %10s %8s %8s %8s" % ("margin", "median s", "worst s", "never",
                                     "cand/frm"))
    for m in MARGINS:
        times, never, cand, frames = [], 0, 0, 0
        for (a, b, ref) in pairs:
            t = make(m)
            cut = len(a[1])
            t.run(np.concatenate((a[1], b[1])))
            r = relock_time(t, cut, ref)
            if r is None:
                never += 1
                times.append(999.0)
            else:
                times.append(r)
            cand += t.cand_total
            frames += t.frame_index
        arr = np.array(times)
        print("%-8.2f %10.2f %8.2f %8d %8.1f"
              % (m, float(np.median(arr)), float(arr.max()), never,
                 cand / max(1, frames)), flush=True)

    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

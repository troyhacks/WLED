#!/usr/bin/env python3
"""THE VETO'S NUMBERS, UNDER BOTH PLLS.

ar_tempo.h's veto comment quotes:

    "the re-lock measured 16.6 s against 14.1 s without the veto.
     The hit rate does not move."

Those numbers came from _fw_vetoscope.py, which runs the tracker with the
DEFAULT PLL - the 0.5 % CLAMP.  The firmware now ships a PROPORTIONAL PLL
(AR_TEMPO_PLL_GAIN = 0.10), so the veto is being asked to do the same job on
a different rate response.

This measures the re-lock, the hit rate and the wander for the veto ON and
OFF, under BOTH PLLs, on the same corpus.

    python tools/ar_sim/_fw_relockgain.py --windows 20 --pairs 10
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

NARROW = 2.0
EVERY = 4
NET = 32
TOL_BPM = 1.0
HOLD_FRAMES = 281


def make(margin, gain):
    t = P.Tracker()
    t.narrow_bpm = NARROW
    t.search_every = EVERY
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = NET
    t.net_margin = margin
    t.veto_on_collapse = True
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
    ap.add_argument("--windows", type=int, default=20)
    ap.add_argument("--pairs", type=int, default=10)
    args = ap.parse_args()

    t0 = time.time()
    items = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, 80.0, 320.0)
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

    for gain, gname in ((0.0, "clamp 0.5%"), (0.10, "proportional 0.10")):
        for margin, mname in ((0.0, "no veto"), (0.20, "veto 0.20")):
            hits, wand, jumps, bigs, veto = [], [], [], [], []
            for (k, odf, ons) in items:
                t = make(margin, gain)
                t.run(odf)
                if not t.beats:
                    hits.append(0.0)
                    wand.append(0.0)
                    jumps.append(0)
                    bigs.append(0.0)
                    veto.append(0.0)
                    continue
                f, _off = P.hit_with_offset(np.array(t.beats) / P.RATE, ons)
                hits.append(f)
                w, j, b = wander(t)
                wand.append(w)
                jumps.append(j)
                bigs.append(b)
                veto.append(t.net_vetoes)
            times, never = [], 0
            for (a, b, ref) in pairs:
                t = make(margin, gain)
                cut = len(a[1])
                t.run(np.concatenate((a[1], b[1])))
                r = relock_time(t, cut, ref)
                if r is None:
                    never += 1
                    times.append(999.0)
                else:
                    times.append(r)
            arr = np.array(times)
            print("%-20s %-10s  hit %.3f  wander %.3f  jumps %4.1f  biggest"
                  " %.2f  vetoed %4.1f | relock median %6.2f s  worst %6.2f"
                  "  never %d"
                  % (gname, mname, float(np.median(hits)),
                     float(np.mean(wand)), float(np.mean(jumps)),
                     float(np.mean(bigs)), float(np.mean(veto)),
                     float(np.median(arr)), float(arr.max()), never),
                  flush=True)
        print()


if __name__ == "__main__":
    sys.exit(main())

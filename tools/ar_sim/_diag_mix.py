#!/usr/bin/env python3
"""Is the comb's score peaked at the TRUE rate, or does it ride the top?

A: the score-vs-rate curve on single tracks.
B: the same tracker on a DJ-style MIX of different tracks.

    python tools/ar_sim/_diag_mix.py --windows 40 --mix 12
"""

import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402

ORACLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_oracle")


def oracle_bpm(key):
    p = os.path.join(ORACLE, key + ".json")
    if not os.path.exists(p):
        return 0.0
    with open(p, encoding="utf-8") as f:
        return float(json.load(f).get("bpm", 0.0))


def make(margin=0.70, gain=0.20, prior=0.6):
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
    t.prior_in_narrow = False
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=40)
    ap.add_argument("--mix", type=int, default=12)
    ap.add_argument("--band-lo", type=float, default=90.0)
    ap.add_argument("--band-hi", type=float, default=320.0)
    args = ap.parse_args()

    keys = _cache.keys()[:args.windows]
    grid = P.BPM_LO + np.arange(P.CAND_COUNT) * P.CAND_STEP

    # ---- A: the score-vs-rate curve ------------------------------------
    print("A: the raw score's argmax, vs librosa's rate")
    print("%-8s %8s %8s %8s %8s %8s"
          % ("librosa", "argmax", "track", "s@lib", "s@arg", "s@175"))
    n = 0
    tops = []
    for k in keys:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, args.band_lo, args.band_hi)
        if len(odf) < 1200:
            continue
        t = make()
        t.run(odf)
        if len(t.sweep_frames) == 0:
            continue
        sc, _ph = t.comb_scores_all(t.sweep_frames, t.sweep_str, grid)
        am = float(grid[int(np.argmax(sc))])
        lb = oracle_bpm(k)
        i_lb = int(np.argmin(np.abs(grid - lb))) if lb > 0 else 0
        i_175 = int(np.argmin(np.abs(grid - 175.0)))
        tops.append(am)
        print("%8.2f %8.2f %8.2f %8.4f %8.4f %8.4f"
              % (lb, am, t.bpm, sc[i_lb], sc.max(), sc[i_175]))
        n += 1
        if n >= 25:
            break
    tops = np.array(tops)
    print()
    print("argmax over %d windows: median %.2f  mean %.2f  min %.2f  max %.2f"
          % (len(tops), np.median(tops), tops.mean(), tops.min(), tops.max()))
    print("at or above 165: %.0f%%" % (100.0 * (tops >= 165).mean()))
    print()

    # ---- B: a DJ-style mix ----------------------------------------------
    print("B: the tracker on a MIX of %d different tracks" % args.mix)
    mkeys = _cache.keys()[:400]
    picks = []
    seen = set()
    for k in mkeys:
        tr = k.split("@")[0]
        if tr in seen:
            continue
        seen.add(tr)
        picks.append(k)
        if len(picks) >= args.mix:
            break
    mix = []
    for k in picks:
        x = _cache.load(k)
        if x is None:
            continue
        mix.append(np.asarray(x, dtype=np.float64))
        mix.append(np.zeros(int(0.05 * _cache.CACHE_RATE)))   # 50 ms gap
    audio = np.concatenate(mix)
    odf = B.firmware_odf(audio, args.band_lo, args.band_hi)
    t = make()
    t.run(odf)
    log = np.array(t.bpm_log)
    lib = [oracle_bpm(k) for k in picks]
    print("the mix's librosa rates: %s"
          % " ".join("%.0f" % v for v in lib))
    print()
    print("%6s %8s %8s" % ("t(s)", "bpm", "score"))
    step = max(1, len(log) // 40)
    for i in range(0, len(log), step):
        print("%6.1f %8.2f %8.3f" % (i / P.RATE, log[i], 0.0))
    print()
    print("published rate: median %.2f  mean %.2f  min %.2f  max %.2f"
          % (np.median(log[log > 0]), log[log > 0].mean(),
             log[log > 0].min(), log[log > 0].max()))


if __name__ == "__main__":
    sys.exit(main())

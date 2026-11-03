#!/usr/bin/env python3
"""THE CEILING: how many raw onsets CAN a beat grid hit?

The tracker is scored against the raw onsets - the 40-160 Hz kicks and a wide
20-2000 Hz detector - and the number came out lower than the tracker used to
score.  That is a fact and it needs an explanation, and the explanation has
to be measured, not asserted.

The raw onsets are NOT the beat.  A house track has a kick on every beat, a
snare on two, a hat on every eighth, and an off-beat open hat; the raw
onsets are all of them.  So a grid can only hit the fraction of them that
ARE on the beat, and that fraction is a CEILING.  A tracker that scores
above the ceiling is not a better tracker - it is a grid that is covering
onsets by being fast enough to sit on the off-beats as well.

librosa's beat grid is an independent beat grid, so it measures the ceiling:
what an independent implementation achieves against the same onsets.

    python tools/ar_sim/_fw_ceiling.py --windows 120
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
import _oracle_librosa as O                        # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402


def make(margin=0.70, gain=0.10, prior=0.6, narrow_prior=False,
         lo=80.0, hi=320.0):
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=120)
    ap.add_argument("--lo", type=float, default=80.0)
    ap.add_argument("--hi", type=float, default=320.0)
    ap.add_argument("--margin", type=float, default=0.70)
    ap.add_argument("--gain", type=float, default=0.10)
    ap.add_argument("--prior", type=float, default=0.6)
    ap.add_argument("--narrow-prior", type=int, default=0)
    ap.add_argument("--show", type=int, default=12)
    args = ap.parse_args()

    t0 = time.time()
    rows = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        wons = B.wide_onsets(x)
        orc = O.load(k)
        if orc is None or len(ons) < 4 or len(wons) < 4:
            continue
        odf = B.firmware_odf(x, args.lo, args.hi)
        if len(odf) < 1200:
            continue
        t = make(args.margin, args.gain, args.prior,
                 bool(args.narrow_prior), args.lo, args.hi)
        t.run(odf)
        if not t.beats:
            continue
        tb = np.array(t.beats) / P.RATE          # the tracker's beats, seconds
        lb = np.array(orc["beats"], dtype=float)  # librosa's beats, seconds
        if len(lb) < 4:
            continue
        rows.append({
            "key": k,
            "bpm": t.bpm,
            "lbpm": orc["bpm"],
            "lib_kick": P.hit_with_offset(lb, ons)[0],
            "lib_wide": P.hit_with_offset(lb, wons)[0],
            "trk_kick": P.hit_with_offset(tb, ons)[0],
            "trk_wide": P.hit_with_offset(tb, wons)[0],
            "trk_lib": P.hit_with_offset(tb, lb)[0],
            "nons": len(ons),
        })
    print("%d windows   %.0f s" % (len(rows), time.time() - t0), flush=True)
    print()

    def col(name):
        return np.array([r[name] for r in rows], dtype=float)

    print("%-34s %8s %8s %8s" % ("", "median", "mean", "min"))
    print("-" * 62)
    for name, label in (("lib_kick", "THE CEILING, kick onsets"),
                        ("trk_kick", "the tracker,  kick onsets"),
                        ("lib_wide", "THE CEILING, wide onsets"),
                        ("trk_wide", "the tracker,  wide onsets"),
                        ("trk_lib",  "the tracker vs librosa's beats")):
        c = col(name)
        print("%-34s %8.4f %8.4f %8.4f"
              % (label, float(np.median(c)), float(c.mean()), float(c.min())))
    print()

    # the rate, against librosa's rate, allowing the metrical relation
    errs, levels = [], []
    for r in rows:
        if r["lbpm"] <= 0.0 or r["bpm"] <= 0.0:
            continue
        d = [abs(r["bpm"] - r["lbpm"]),
             abs(r["bpm"] - 2.0 * r["lbpm"]),
             abs(r["bpm"] - r["lbpm"] * 0.5)]
        errs.append(min(d))
        levels.append(int(np.argmin(d)))
    errs = np.array(errs)
    print("the rate against librosa's rate")
    print("   median %.3f BPM   mean %.3f BPM   p90 %.3f BPM"
          % (float(np.median(errs)), float(errs.mean()),
             float(np.percentile(errs, 90))))
    lv = np.array(levels)
    for i, nm in enumerate(("same", "double", "half")):
        print("   %-8s %5.1f %%" % (nm, 100.0 * float(np.mean(lv == i))))
    print()

    # the interesting rows: the tracker BEATS the ceiling
    print("the windows where the tracker beats the ceiling")
    print("%-46s %8s %8s %8s %8s"
          % ("window", "bpm", "lib bpm", "lib kick", "trk kick"))
    print("-" * 82)
    better = [r for r in rows if r["trk_kick"] > r["lib_kick"] + 0.02]
    better.sort(key=lambda r: -(r["trk_kick"] - r["lib_kick"]))
    for r in better[:args.show]:
        print("%-46s %8.2f %8.2f %8.4f %8.4f"
              % (r["key"][:44], r["bpm"], r["lbpm"],
                 r["lib_kick"], r["trk_kick"]))
    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

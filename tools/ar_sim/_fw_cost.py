#!/usr/bin/env python3
"""WHAT THE COMB COSTS, AND WHAT IT BUYS.

The firmware's comb scores candidate rates against every onset in the
last 12 s.  Measured on the P4, the tempo stage used to average about a
millisecond a frame - and the whole frame's budget is 10.67 ms.  That is
a tenth of the board's time spent on a feature most boards will never
look at.

The cost is O(candidates x peaks) per frame.  There are three ways to cut
it, and this measures each of them - the hit rate AND the cost - because
a cut that costs accuracy is not a cut.

  1. FEWER PEAKS.  The picker over-detects badly on house music: about 21
     onsets a second where the beat is 2.1 a second, so ten onsets per
     beat, and the comb's window is always full at the 256-onset ring
     limit.  Either the picker is made stricter (a higher delta) or the
     comb is given only the STRONGEST peaks (top_n).
  2. FEWER CANDIDATES.  A coarser rate step halves the whole-band search.
  3. A NARROWER SEARCH.  Once the grid is running, the search does not
     have to look at the whole band again: it looks at the rate it
     believes, plus or minus a couple of BPM, every few frames.  The
     whole-band search becomes a SAFETY NET that runs every `net` frames.

The cost is MEASURED, not modelled: the port counts the candidates it
scores, and the count is divided by the frames it took.

    python tools/ar_sim/_fw_cost.py --limit 200
"""

import argparse
import math
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402

# The variants.  Each is a dict of the tracker's knobs.
#
#   delta    the picker's rise threshold
#   floor    the picker's floor, as a fraction of the ODF's running max
#   top_n    the comb sees only the N strongest onsets (0 = all of them)
#   step     the candidate step, in BPM
#   narrow   the narrow search's half-width, in BPM (0 = always the whole band)
#   every    how many frames between narrow searches, once locked
#   edge     the winner-at-the-edge rule
#   collapse the score-collapsed rule
#   net      a whole-band search every N frames (0 = never)
#
VARIANTS = [
    # the firmware before this change: a whole-band search every eight frames
    ("base",       dict(delta=0.35, floor=0.06, top_n=0, step=0.25, narrow=0.0,
                        every=4, edge=False, collapse=False, net=0)),
    # the design this change adopts
    ("narrow+net32", dict(delta=0.35, floor=0.06, top_n=0, step=0.25, narrow=2.0,
                          every=4, edge=False, collapse=True, net=32)),
    ("narrow+net16", dict(delta=0.35, floor=0.06, top_n=0, step=0.25, narrow=2.0,
                          every=4, edge=False, collapse=True, net=16)),
    ("narrow+net64", dict(delta=0.35, floor=0.06, top_n=0, step=0.25, narrow=2.0,
                          every=4, edge=False, collapse=True, net=64)),
    # the parts, on their own
    ("narrow only", dict(delta=0.35, floor=0.06, top_n=0, step=0.25, narrow=2.0,
                         every=4, edge=True, collapse=False, net=0)),
    ("narrow+coll", dict(delta=0.35, floor=0.06, top_n=0, step=0.25, narrow=2.0,
                         every=4, edge=True, collapse=True, net=0)),
    # the other cost levers
    ("net32+top64", dict(delta=0.35, floor=0.06, top_n=64, step=0.25, narrow=2.0,
                         every=4, edge=False, collapse=True, net=32)),
    ("net32+d.5",   dict(delta=0.50, floor=0.06, top_n=0, step=0.25, narrow=2.0,
                         every=4, edge=False, collapse=True, net=32)),
    ("net32+step.5", dict(delta=0.35, floor=0.06, top_n=0, step=0.50, narrow=2.0,
                          every=4, edge=False, collapse=True, net=32)),
]


def build_tracker(v):
    """A tracker with one variant's knobs."""
    t = P.Tracker()
    t.delta = v["delta"]
    t.floor = v["floor"]
    t.top_n = v["top_n"]
    t.cand_step = v["step"]
    t.cand_count = int(round((P.BPM_HI - P.BPM_LO) / v["step"])) + 1
    t.prior = np.array([
        P.PRIOR_WEIGHT * (math.log((P.BPM_LO + i * v["step"]) / P.PRIOR_BPM)
                          / P.PRIOR_SIGMA) ** 2
        for i in range(t.cand_count)])
    t.narrow_bpm = v["narrow"]
    t.search_every = v["every"]
    t.edge_rule = v["edge"]
    t.collapse_trigger = v["collapse"]
    t.full_every = v["net"]
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=200)
    ap.add_argument("--lo", type=float, default=40.0)
    ap.add_argument("--hi", type=float, default=160.0)
    args = ap.parse_args()

    keys = _cache.keys()[:args.limit]
    hits = {v[0]: [] for v in VARIANTS}
    peaks = {v[0]: [] for v in VARIANTS}
    bpms = {v[0]: [] for v in VARIANTS}
    scores = {v[0]: [] for v in VARIANTS}
    cost = {v[0]: [] for v in VARIANTS}
    t0 = time.time()
    n = 0
    for k in keys:
        x = _cache.load(k)
        if x is None:
            continue
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        if len(ons) < 4:
            continue
        odf = B.firmware_odf(x, args.lo, args.hi)
        if len(odf) < 1200:
            continue
        for (name, v) in VARIANTS:
            t = build_tracker(v)
            t.run(odf)
            if not t.beats:
                hits[name].append(0.0)
                peaks[name].append(0)
                bpms[name].append(0.0)
                scores[name].append(0.0)
                cost[name].append(0.0)
                continue
            f, _off = P.hit_with_offset(np.array(t.beats) / P.RATE, ons)
            hits[name].append(f)
            peaks[name].append(len(t.sweep_frames) if t.sweep_frames is not None
                               else 0)
            bpms[name].append(t.bpm)
            scores[name].append(t.score)
            cost[name].append(t.cand_total / float(max(1, t.frame_index)))
        n += 1
        if n % 20 == 0:
            print("  %d/%d  (%.0f s)" % (n, len(keys), time.time() - t0),
                  flush=True)

    print()
    print("%d windows   %.0f s   band %g-%g Hz"
          % (n, time.time() - t0, args.lo, args.hi))
    print()
    print("%-16s %7s %6s %6s | %6s | %6s | %7s"
          % ("variant", "median", ">=0.5", ">=0.8", "score", "peaks", "cand/frm"))
    base_cost = None
    for (name, v) in VARIANTS:
        a = np.array(hits[name])
        pk = np.array(peaks[name])
        sc = np.array(scores[name])
        bm = np.array(bpms[name])
        c = float(np.mean(cost[name]))
        if name == "base":
            base_cost = c
        print("%-16s %7.3f %5.1f%% %5.1f%% | %6.3f | %6.1f | %6.1f (x%.2f) bpm %.1f"
              % (name, float(np.median(a)), 100.0 * float((a >= 0.5).mean()),
                 100.0 * float((a >= 0.8).mean()), float(np.median(sc)),
                 float(pk.mean()), c,
                 (c / base_cost) if base_cost else 1.0, float(np.median(bm))))


if __name__ == "__main__":
    sys.exit(main())

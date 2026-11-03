#!/usr/bin/env python3
"""IS THE RATE STABLE, AND WHAT MAKES IT MOVE?

THE QUESTION THIS ANSWERS
-------------------------
The board was asked for its tempo and it answered 125.00, then 131.00,
then 125.00 again.  Two searches, two answers, and the PLL integrates
both, so the rate walks between them.

The whole-band search finds the best rate in the WHOLE band, with the
prior's penalty applied.  The narrow search finds the best rate in a
window around what the grid already believes.  When they disagree, the
grid is dragged between them - and the narrow search is the one that
runs most often, so the rate ends up wherever the pair of them average
out.

So this measures, for each band and each design:
  - the hit rate, against both ground truths
  - the score, which is the comb's own confidence
  - the WANDER: how far the rate moves after it has settled

    python tools/ar_sim/_fw_stable.py --limit 60
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

BANDS = [(40.0, 160.0), (80.0, 320.0)]

DESIGNS = [
    # name,             narrow, edge, collapse, net
    ("whole (today)",   0.0, False, False, 0),
    ("narrow+net32",    2.0, False, True,  32),
]


def tracker_for(d, odf):
    t = P.Tracker()
    t.narrow_bpm = d[1]
    t.edge_rule = d[2]
    t.collapse_trigger = d[3]
    t.full_every = d[4]
    t.run(odf)
    return t


def wander(t):
    """How much the rate moves after it has settled.

    The second half of the window: by then the tracker has locked and the
    music is steady.  The standard deviation of the rate over that half,
    in BPM, and the number of times it jumped by more than half a BPM.
    """
    log = np.array(t.bpm_log)
    if len(log) < 100:
        return 0.0, 0
    half = log[len(log) // 2:]
    half = half[half > 0.0]
    if len(half) < 10:
        return 0.0, 0
    jumps = int(np.sum(np.abs(np.diff(half)) > 0.5))
    return float(np.std(half)), jumps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=60)
    args = ap.parse_args()

    keys = _cache.keys()[:args.limit]
    print("%-10s %-16s %8s %8s %8s %8s %8s"
          % ("band", "design", "hit", "score", "wander", "jumps", "cand/frm"))
    t0 = time.time()
    n = 0
    for k in keys:
        x = _cache.load(k)
        if x is None:
            continue
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        if len(ons) < 4:
            continue
        for (lo, hi) in BANDS:
            odf = B.firmware_odf(x, lo, hi)
            if len(odf) < 1200:
                continue
            for d in DESIGNS:
                t = tracker_for(d, odf)
                if not t.beats:
                    print("%-10s %-16s   (no beats)" % ("%g-%g" % (lo, hi),
                                                         d[0]))
                    continue
                f, _off = P.hit_with_offset(np.array(t.beats) / P.RATE, ons)
                w, j = wander(t)
                c = t.cand_total / float(max(1, t.frame_index))
                print("%-10s %-16s %8.3f %8.3f %8.3f %8d %8.1f"
                      % ("%g-%g" % (lo, hi), d[0], f, t.score, w, j, c))
        n += 1
        if n % 10 == 0:
            print("  ... %d/%d  (%.0f s)" % (n, len(keys), time.time() - t0),
                  flush=True)

    return 0


if __name__ == "__main__":
    sys.exit(main())

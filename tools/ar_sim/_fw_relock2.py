#!/usr/bin/env python3
"""HOW FAST DOES IT FOLLOW - AND WHAT DOES THE VETO COST?

The margin sweep says a big margin makes the tracker steady: at 0.70 the
far candidate never moves the grid at all, and the wander and the jumps go
to zero.  The price of that steadiness is the other half of the question:
if the far candidate may never move the grid, the ONLY way the tracker can
follow a new tempo is the COLLAPSE path - the narrow search's score falls
below the minimum, and then a whole-band search looks everywhere.

So this cuts two real tracks against each other and measures how long the
tracker keeps reporting the OLD tempo after the music has changed:

  A's ODF, then B's ODF.  The reference for B is the rate a tracker finds
  when it is given B ALONE.  The re-lock time is the first moment after
  the cut at which the tracker is on B's rate AND STAYS there for three
  seconds - a tracker that touches the right rate and drifts off again has
  not re-locked, it has wobbled.

    python tools/ar_sim/_fw_relock2.py --windows 36 --pairs 16
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

TOL_BPM = 1.0
HOLD_FRAMES = 281        # 3.0 s


def make_tracker(margin, gain, hard=False):
    """The shipped configuration, with the two knobs the sweep moves."""
    t = P.Tracker()
    t.narrow_bpm = 2.0
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = 32
    t.net_margin = margin
    t.pll_gain = gain
    t.hard_far = hard
    return t


def reference_bpm(odf):
    """The rate a tracker finds when it is given this track alone."""
    t = P.Tracker()
    t.narrow_bpm = 0.0
    t.run(odf)
    if not t.locked or t.bpm <= 0.0:
        return None
    return t.bpm


def relock_time(t, cut_frame, ref_bpm):
    log = t.bpm_log
    n = len(log)
    first = None
    for f in range(cut_frame, n):
        if t.locked and abs(log[f] - ref_bpm) <= TOL_BPM:
            first = f
            break
    if first is None:
        return None
    end = min(n, first + HOLD_FRAMES)
    for f in range(first, end):
        if not t.locked or abs(log[f] - ref_bpm) > 2.0 * TOL_BPM:
            return None
    return (first - cut_frame) / P.RATE


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=36)
    ap.add_argument("--pairs", type=int, default=16)
    ap.add_argument("--lo", type=float, default=80.0)
    ap.add_argument("--hi", type=float, default=320.0)
    ap.add_argument("--margins", type=str, default="0.20,0.45,0.70")
    ap.add_argument("--gain", type=float, default=0.10)
    ap.add_argument("--hard", action="store_true")
    args = ap.parse_args()
    margins = [float(v) for v in args.margins.split(",")]

    t0 = time.time()
    items = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, args.lo, args.hi)
        if len(odf) < 1200:
            continue
        bpm = reference_bpm(odf)
        if bpm is None:
            continue
        items.append((k, odf, bpm))
    print("%d windows   %.0f s" % (len(items), time.time() - t0), flush=True)

    pairs = []
    for i in range(len(items) - 1):
        for j in range(i + 1, len(items)):
            if abs(items[j][2] - items[i][2]) < 6.0:
                continue
            pairs.append((items[i], items[j]))
            break
        if len(pairs) >= args.pairs:
            break
    print("%d pairs" % len(pairs))
    print()
    print("%-8s %10s %10s %10s %8s %8s"
          % ("margin", "median s", "mean s", "worst s", "never", "far"))
    for m in margins:
        times = []
        never = 0
        fars = []
        for ((ka, odf_a, _ba), (kb, odf_b, bpm_b)) in pairs:
            t = make_tracker(m, args.gain, args.hard)
            cut = len(odf_a)
            t.run(np.concatenate((odf_a, odf_b)))
            r = relock_time(t, cut, bpm_b)
            if r is None:
                never += 1
                times.append(999.0)
            else:
                times.append(r)
            fars.append(t.net_far)
        a = np.array(times)
        print("%-8.2f %10.2f %10.2f %10.2f %8d %8.1f"
              % (m, float(np.median(a)), float(a.mean()), float(a.max()),
                 never, float(np.mean(fars))), flush=True)
    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

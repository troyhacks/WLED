#!/usr/bin/env python3
"""HOW FAST DOES THE TRACKER NOTICE THAT THE MUSIC CHANGED?

THE NARROW SEARCH'S ONE FAILURE MODE
------------------------------------
Once the grid is running, the search only looks at the rate the grid
believes, plus or minus a couple of BPM.  That is a tenth of the work of
the whole-band search, and it finds the same rate - on a track that keeps
its tempo.

But a DJ does not play one track at one tempo for ever.  When the music
changes, the narrow search is looking in the wrong place: the new beat is
nowhere near the old one.  The question this answers is how long the
tracker keeps reporting the OLD tempo after the music has changed, and
which of the ways out of that costs least.

THE WAYS OUT
------------
  EDGE      the narrow search's winner sits at the edge of its window, so
            the rate may be just beyond it.
  COLLAPSE  the narrow search's score fell below the minimum - the beat it
            was tracking is not there any more.
  NET       a whole-band search every N frames, whatever the score says.

THE MEASUREMENT
---------------
Two tracks are cut against each other: A's ODF, then B's ODF.  The
reference for B is the rate a tracker finds when it is given B ALONE -
so the reference is the tracker's own answer for B, measured on 40 s of B
rather than on the 2 s after the cut.

The re-lock time is the first moment after the cut at which the tracker
is on B's rate AND STAYS there - because a tracker that touches the right
rate and then drifts off again has not re-locked, it has wobbled.

    python tools/ar_sim/_fw_relock.py --windows 36 --pairs 16
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

# The variants.  Each is the tracker's knobs.
#
#   narrow   the narrow search's half-width in BPM (0 = always whole-band)
#   edge     the winner-at-the-edge rule
#   collapse the score-collapsed rule
#   net      a whole-band search every N frames (0 = never)
#
VARIANTS = [
    ("whole (today)",  dict(narrow=0.0, edge=False, collapse=False, net=0)),
    ("narrow+net32",   dict(narrow=2.0, edge=False, collapse=True,  net=32)),
    ("narrow+net16",   dict(narrow=2.0, edge=False, collapse=True,  net=16)),
    ("narrow+net64",   dict(narrow=2.0, edge=False, collapse=True,  net=64)),
    ("narrow+net32+ed", dict(narrow=2.0, edge=True, collapse=True,  net=32)),
    ("narrow+edge",    dict(narrow=2.0, edge=True,  collapse=False, net=0)),
    ("narrow+net32noc", dict(narrow=2.0, edge=False, collapse=False, net=32)),
]

TOL_BPM = 1.0        # "on the right rate"
HOLD_FRAMES = 281    # ... and still there 3 s later (281 frames = 3.0 s)


def make_tracker(v):
    t = P.Tracker()
    t.narrow_bpm = v["narrow"]
    t.edge_rule = v["edge"]
    t.collapse_trigger = v["collapse"]
    t.full_every = v["net"]
    return t


def reference_bpm(odf):
    """The rate a tracker finds when it is given this track alone."""
    t = P.Tracker()               # the whole-band search - the old behaviour
    t.narrow_bpm = 0.0
    t.run(odf)
    if not t.locked or t.bpm <= 0.0:
        return None
    return t.bpm


def relock_time(t, cut_frame, ref_bpm):
    """The seconds after the cut at which the tracker is on the new rate.

    None if it never gets there and stays there.
    """
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
    args = ap.parse_args()

    # The ODFs first: each one costs a few seconds, and the tracker runs
    # below are cheap.  So the expensive thing is done once.
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
        print("  %-44s %6.1f bpm   %.0f s" % (k[:44], bpm, time.time() - t0),
              flush=True)
    if len(items) < 4:
        print("not enough usable windows")
        return 1

    # The pairs: two windows whose rates are FAR APART.  A change from 128
    # to 129 is not a test of anything.
    pairs = []
    for i in range(len(items) - 1):
        for j in range(i + 1, len(items)):
            if abs(items[j][2] - items[i][2]) < 6.0:
                continue
            pairs.append((items[i], items[j]))
            break
        if len(pairs) >= args.pairs:
            break

    print()
    print("%d pairs from %d windows   band %g-%g Hz   %.0f s"
          % (len(pairs), len(items), args.lo, args.hi, time.time() - t0))
    print()
    print("%-16s %10s %8s %8s %8s %8s"
          % ("variant", "median s", "worst s", "never", "cand/frm", "x"))
    base_cost = None
    for (name, v) in VARIANTS:
        times = []
        never = 0
        cand = 0.0
        frames = 0
        for ((ka, odf_a, _ba), (kb, odf_b, bpm_b)) in pairs:
            t = make_tracker(v)
            cut = len(odf_a)
            t.run(np.concatenate((odf_a, odf_b)))
            r = relock_time(t, cut, bpm_b)
            if r is None:
                never += 1
                times.append(999.0)
            else:
                times.append(r)
            cand += t.cand_total
            frames += t.frame_index
        a = np.array(times)
        c = cand / max(1, frames)
        if base_cost is None:
            base_cost = c
        print("%-16s %10.2f %8.2f %8d %8.1f %8.2f"
              % (name, float(np.median(a)), float(a.max()), never, c,
                 c / base_cost if base_cost else 1.0))

    return 0


if __name__ == "__main__":
    sys.exit(main())

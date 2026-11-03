#!/usr/bin/env python3
"""THE KERNEL'S ROUNDING: DOES IT CHANGE THE ANSWER?

The comb's kernel is a box of half-width tol, and the half-width in BINS is
rounded to a whole number.  A bin is period/64 wide - 7 ms at 128 BPM - so
the tolerance the tracker actually applies moves by up to half a bin, which
is about 5 % of the 70 ms it asks for.

A wider box catches more of the smear of a WRONG period, so the candidate
that happens to round UP scores higher than its neighbour that rounds DOWN.
That is a sawtooth in the score profile, and the veto compares two scores
across the sawtooth.

So: does it matter?  This measures the winner with the integer kernel and
with the continuous one - the same box with no rounding - over the corpus,
and the hit rate of both.

    python tools/ar_sim/_fw_kernel.py --windows 60
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


def make(exact, margin=0.20, gain=0.10):
    t = P.Tracker()
    t.narrow_bpm = 2.0
    t.search_every = 4
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = 32
    t.net_margin = margin
    t.pll_gain = gain
    t.exact_kernel = exact
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=60)
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

    hits = {False: [], True: []}
    diffs = []
    for (k, odf, ons) in items:
        line = []
        for exact in (False, True):
            t = make(exact)
            t.run(odf)
            if not t.beats:
                hits[exact].append(0.0)
                line.append(0.0)
                continue
            f, _off = P.hit_with_offset(np.array(t.beats) / P.RATE, ons)
            hits[exact].append(f)
            line.append(f)
        diffs.append(line[1] - line[0])

    a = np.array(hits[False])
    b = np.array(hits[True])
    d = np.array(diffs)
    print()
    print("integer kernel : median %.4f  mean %.4f" % (np.median(a), a.mean()))
    print("continuous      : median %.4f  mean %.4f" % (np.median(b), b.mean()))
    print("difference      : median %+.4f  mean %+.4f  worst %+.4f"
          % (np.median(d), d.mean(), d.min()))
    print("windows worse   : %d of %d" % (int((d < -1e-9).sum()), len(d)))
    print("windows better  : %d of %d" % (int((d > 1e-9).sum()), len(d)))
    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

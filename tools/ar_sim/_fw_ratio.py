#!/usr/bin/env python3
"""HOW MUCH BETTER IS THE FAR CANDIDATE, REALLY?

The veto compares the far rate's score with the local rate's score, and
lets the far one move the grid only if it is CLEARLY better.  "Clearly"
has to be a number, and the number has to be measured - and the
measurement has to answer the question the veto is actually asking:

    when the far candidate is much better than the local one, is that
    because the music changed - or because the comb has a second peak?

So this collects the ratio (far score / local score) from every whole-band
sweep, over the corpus, and over CORPUS CUT AGAINST ITSELF - two
different tracks with different tempos, cut together - and reports the
two distributions side by side.

    python tools/ar_sim/_fw_ratio.py --windows 40 --pairs 10
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


def make(margin=0.70, gain=0.10, prior=0.6, narrow_prior=False):
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
    t.trace = []
    return t


def report(name, ratios):
    r = np.array(ratios)
    if not len(r):
        print("%s: nothing" % name)
        return
    print("%-28s %6d sweeps   median %.3f   p75 %.3f   p90 %.3f   "
          "p99 %.3f   max %.3f"
          % (name, len(r), float(np.median(r)),
             float(np.percentile(r, 75)), float(np.percentile(r, 90)),
             float(np.percentile(r, 99)), float(r.max())))
    for th in (0.20, 0.45, 0.70, 1.00, 1.50, 2.00):
        print("      above %.2f: %6d  (%5.2f %%)"
              % (th, int(np.sum(r > th)), 100.0 * np.mean(r > th)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=40)
    ap.add_argument("--pairs", type=int, default=10)
    ap.add_argument("--lo", type=float, default=80.0)
    ap.add_argument("--hi", type=float, default=320.0)
    ap.add_argument("--margin", type=float, default=0.70)
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
        items.append((k, odf))
    print("%d windows   %.0f s" % (len(items), time.time() - t0), flush=True)

    same = []
    for (k, odf) in items:
        t = make(args.margin)
        t.run(odf)
        same.extend(r["ratio"] for r in t.trace)
    report("THE SAME MUSIC", same)

    # the pairs: different tracks, at least 6 BPM apart
    def reference(odf):
        t = P.Tracker()
        t.narrow_bpm = 0.0
        t.run(odf)
        return t.bpm if t.locked else None

    refs = {k: reference(odf) for (k, odf) in items}
    pairs = []
    for i in range(len(items)):
        for j in range(len(items)):
            if items[j][0].split(" @")[0] == items[i][0].split(" @")[0]:
                continue
            if refs.get(items[j][0]) is None or refs.get(items[i][0]) is None:
                continue
            if abs(refs[items[j][0]] - refs[items[i][0]]) < 6.0:
                continue
            pairs.append((items[i], items[j]))
            break
        if len(pairs) >= args.pairs:
            break
    print("%d pairs of different tracks" % len(pairs), flush=True)

    after = []
    before = []
    for ((ka, odf_a), (kb, odf_b)) in pairs:
        cut = len(odf_a)
        t = make(args.margin)
        t.run(np.concatenate((odf_a, odf_b)))
        for r in t.trace:
            if r["frame"] < cut:
                before.append(r["ratio"])
            else:
                after.append(r["ratio"])
    report("BEFORE THE CUT", before)
    report("AFTER THE CUT", after)
    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

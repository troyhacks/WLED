#!/usr/bin/env python3
"""WHAT ACTUALLY HAPPENS AFTER A TRACK CHANGE.

The re-lock was measured as "the published rate lands on the new track's
rate and stays there".  Half of them never did.  This asks the question
underneath that one: what did the tracker DO?

For each pair of DIFFERENT tracks it prints

  A, B          the two tracks' own rates
  the far candidate's rate, just after the cut and five seconds later
  the published rate, just after the cut, five seconds later and at the end

because "it never re-locked" is a symptom, and the cause is in there
somewhere: is the far candidate the new tempo at all, or is it a rate that
has nothing to do with the new music?

    python tools/ar_sim/_fw_relock3.py --windows 40 --pairs 8
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


def reference(odf):
    """The rate the tracker finds when it is given this music on its own."""
    t = P.Tracker()
    t.narrow_bpm = 0.0
    t.run(odf)
    if not t.locked or t.bpm <= 0.0:
        return None
    return t.bpm


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=40)
    ap.add_argument("--pairs", type=int, default=8)
    ap.add_argument("--lo", type=float, default=80.0)
    ap.add_argument("--hi", type=float, default=320.0)
    ap.add_argument("--margin", type=float, default=0.70)
    ap.add_argument("--prior", type=float, default=0.6)
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

    refs = {}
    for (k, odf) in items:
        refs[k] = reference(odf)
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
    print("%d pairs   margin %.2f   prior %.2f"
          % (len(pairs), args.margin, args.prior), flush=True)
    print()
    print("%-9s %-9s | %-9s %-9s | %-8s %-8s | %-8s %-8s %-8s | %s"
          % ("A", "B", "A rate", "B rate", "far@0", "far@5s",
             "pub@0", "pub@5s", "pub@end", "took far"))
    print("-" * 118)

    for ((ka, odf_a), (kb, odf_b)) in pairs:
        cut = len(odf_a)
        t = make(args.margin, prior=args.prior)
        t.run(np.concatenate((odf_a, odf_b)))
        tr = t.trace or []
        after = [r for r in tr if r["frame"] >= cut]
        far0 = after[0]["far_bpm"] if after else 0.0
        far5 = None
        for r in after:
            if r["frame"] >= cut + int(5 * P.RATE):
                far5 = r["far_bpm"]
                break
        blog = np.array(t.bpm_log)
        if len(blog) < cut + 10:
            continue
        took = sum(1 for r in after if r["took_far"])
        print("%-9s %-9s | %9.2f %9.2f | %8.2f %8s | %8.2f %8.2f %8.2f | %d/%d"
              % (ka[:9], kb[:9], refs[ka], refs[kb], far0,
                 ("%.2f" % far5) if far5 is not None else "-",
                 blog[cut], blog[min(len(blog) - 1, cut + int(5 * P.RATE))],
                 blog[-1], took, len(after)), flush=True)
    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

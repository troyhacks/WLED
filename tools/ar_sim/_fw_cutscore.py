#!/usr/bin/env python3
"""WHAT DOES THE SCORE DO WHEN THE MUSIC CHANGES?

AR_TEMPO_MIN_SCORE is the line the whole collapse path stands on: below
it, the beat the tracker was following is declared gone, and a whole-band
search is asked for.  Measured over steady music the narrow search's score
never falls below 0.361 - so with the threshold at 0.10 the collapse can
never fire, and the path is dead code.

But steady music is not the case the collapse exists for.  This cuts two
real tracks against each other and watches the score through the change:

  * the score's floor after the cut - the number a threshold has to be
    BELOW for the collapse to fire at all
  * how long after the cut the score falls under each candidate threshold
  * and, for comparison, how long the re-lock takes

    python tools/ar_sim/_fw_cutscore.py --windows 40 --pairs 12
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

THRESHOLDS = [0.05, 0.10, 0.20, 0.25, 0.30, 0.35, 0.40]
COMB_S = 12.0


def make(margin=0.45, gain=0.10, prior=0.6, narrow_prior=True):
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
    ap.add_argument("--windows", type=int, default=40)
    ap.add_argument("--pairs", type=int, default=12)
    ap.add_argument("--lo", type=float, default=80.0)
    ap.add_argument("--hi", type=float, default=320.0)
    ap.add_argument("--margin", type=float, default=0.45)
    ap.add_argument("--prior", type=float, default=0.6)
    ap.add_argument("--narrow-prior", type=int, default=1)
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

    pairs = []
    for i in range(len(items) - 1):
        for j in range(i + 1, len(items)):
            pairs.append((items[i], items[j]))
            break
        if len(pairs) >= args.pairs:
            break
    print("%d pairs" % len(pairs))
    print()

    floors = []
    fired = {th: [] for th in THRESHOLDS}
    never = {th: 0 for th in THRESHOLDS}
    for ((ka, odf_a), (kb, odf_b)) in pairs:
        cut = len(odf_a)
        t = make(args.margin, 0.10, args.prior, bool(args.narrow_prior))
        t.run(np.concatenate((odf_a, odf_b)))
        sc = np.array(t.score_log)
        if len(sc) < cut + 10:
            continue
        after = sc[cut:]
        floors.append(float(after.min()))
        for th in THRESHOLDS:
            hit = np.nonzero(after < th)[0]
            if len(hit):
                fired[th].append(float(hit[0]) / P.RATE)
            else:
                never[th] += 1
    if not floors:
        print("no usable pairs")
        return 1
    f = np.array(floors)
    print("the score's floor AFTER the cut")
    print("   min %.3f   median %.3f   max %.3f" % (f.min(), np.median(f), f.max()))
    print()
    print("how long after the cut the score falls under a threshold")
    print("   %-8s %10s %10s %10s" % ("threshold", "median s", "mean s", "never"))
    for th in THRESHOLDS:
        a = np.array(fired[th]) if fired[th] else np.array([0.0])
        print("   %-8.2f %10.2f %10.2f %10d"
              % (th, float(np.median(a)), float(a.mean()), never[th]))
    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""THE EVIDENCE GAP: how much score the truth has over its own aliases.

THE QUESTION
------------
The prior is a tie-break, and a tie-break is only useful if it is SMALLER
than the evidence it breaks.  So the question is: on real music, how much
comb score does the TRUE rate have over the rate the estimator mistakes it
for?

The pairs that matter, inside a band that is octave-free and 3:2-free:

    4:3     the residual alias of a narrow band.  It is FASTER than the
            truth (measured: est = 4/3 * truth, never 3/4).
    3:2     the pair a 3:2-free band excludes, measured here for reference.

For every corpus window the truth T is taken at the catalogue-fast level, and
the raw comb score (the prior EXCLUDED) is measured at T, at 4T/3 and at
3T/4.  The gap is score(T) - score(alias).  A POSITIVE gap means the truth
already wins on evidence alone; a NEGATIVE gap means the alias wins and only
the prior can fix it.

The prior's own differential over the same pair is printed beside it, so the
two can be compared:

    dp(T, alias) = w * (ln(alias/C)^2 - ln(T/C)^2) / sigma^2

    python tools/ar_sim/_tempo_gap.py --limit 100
"""

import argparse
import multiprocessing as mp
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402
from _tempo_range import configure, oracle         # noqa: E402


def prior_diff(bpm_a, bpm_b, weight, centre, sigma):
    pa = weight * (np.log(bpm_a / centre) / sigma) ** 2
    pb = weight * (np.log(bpm_b / centre) / sigma) ** 2
    return float(pa - pb)


def one(key):
    o = oracle(key)
    if not o:
        return None
    tb = o[0]
    x = _cache.load(key)
    if x is None:
        return None
    odf = B.firmware_odf(x, 80.0, 320.0)
    configure(lo=118.0, hi=176.0, prior_weight=0.0)
    t = P.Tracker()
    t.run(odf)
    if t.sweep_frames is None or len(t.sweep_frames) < 8:
        return None
    out = []
    for (label, alt) in (("4:3 fast", 4.0 / 3.0), ("4:3 slow", 3.0 / 4.0),
                         ("5:4 fast", 1.25), ("3:2 fast", 1.5),
                         ("half", 0.5), ("octave", 2.0)):
        a = alt * tb
        # the half-time alias lives BELOW the band - that is the whole point
        # of it: it is the out-of-band probe's false-positive candidate.
        if label != "half" and not (118.0 <= a <= 176.0):
            continue
        if not (60.0 <= a <= 180.0):
            continue
        sc, _ = t.comb_scores_all(t.sweep_frames, t.sweep_str,
                                   np.array([tb, a]))
        out.append((label, float(sc[0]), float(sc[1]), float(sc[0] - sc[1])))
    return (tb, out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=100)
    args = ap.parse_args()
    keys = _cache.keys()[:args.limit]

    gaps = {}
    with mp.Pool(8) as pool:
        for r in pool.imap(one, keys):
            if r is None:
                continue
            for (label, st, sa, gap) in r[1]:
                gaps.setdefault(label, []).append(gap)

    print("\n=== the evidence gap: score(truth) - score(alias), prior OFF ===")
    for label, g in sorted(gaps.items()):
        a = np.array(g)
        print("%-10s n=%3d  median %+7.4f  mean %+7.4f  "
              "alias wins in %d/%d (%.0f%%)"
              % (label, len(a), float(np.median(a)), float(a.mean()),
                 int((a < 0).sum()), len(a), 100.0 * (a < 0).mean()))

    print("\n=== the prior's differential over the same pairs ===")
    print("   (the penalty the alias pays MINUS the penalty the truth pays,")
    print("    so a POSITIVE number is the prior helping the truth)")
    for w in (0.1, 0.2, 0.3, 0.6):
        for label, alt in (("4:3 fast", 4.0 / 3.0), ("4:3 slow", 3.0 / 4.0)):
            for tb in (118.0, 125.0, 132.0, 150.0, 165.0, 176.0):
                a = alt * tb
                if not (118.0 <= a <= 176.0):
                    continue
                d = prior_diff(a, tb, w, 125.0, 0.35)
                print("   w%.1f  %-9s truth %6.1f  alias %6.1f  dp %+7.4f"
                      % (w, label, tb, a, d))
        print()

    print("=== the prior's dynamic range across the band 118-176 ===")
    for w in (0.1, 0.2, 0.3, 0.6):
        p = w * (np.log(np.arange(118.0, 176.01, 0.25) / 125.0) / 0.35) ** 2
        print("   w%.1f @125 sigma0.35 : min %+.4f  max %+.4f  range %.4f"
              % (w, p.min(), p.max(), p.max() - p.min()))
    return 0


if __name__ == "__main__":
    sys.exit(main())

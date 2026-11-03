#!/usr/bin/env python3
"""THE HIT RATE BINNED BY THE TRUE TEMPO.

The two populations disagree because they are shaped differently:

  the corpus   120 windows of real house music, truths CONCENTRATED around
                120-135 (median 128), so a prior centred low costs it little
                and a prior centred high costs it little;
  the synthetic 90 tracks, truths SPREAD EVENLY over 118-176, so the prior's
                tax at the top of the band is paid by 40% of them.

A single hit rate per configuration hides exactly the thing that matters:
WHICH TEMPOS each configuration loses.  This bins the corpus by its truth
and reports the hit rate per bin, for the candidate configurations.

    python tools/ar_sim/_tempo_bytempo.py --limit 150
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

LO, HI = 118.0, 176.0
SIGMA = 0.35
CONFIGS = [
    (0.20, 118.0), (0.30, 118.0), (0.40, 125.0), (0.60, 125.0),
]
BINS = [(118, 125), (125, 132), (132, 140), (140, 150), (150, 176)]


def one(key):
    o = oracle(key)
    if not o:
        return None
    tb = float(o[0])
    x = _cache.load(key)
    if x is None:
        return None
    odf = B.firmware_odf(x, 80.0, 320.0)
    out = []
    for (w, c) in CONFIGS:
        configure(lo=LO, hi=HI, prior_weight=w, prior_bpm=c, prior_sigma=SIGMA)
        t = P.Tracker()
        t.run(odf)
        e = t.bpm
        ok = e > 0 and abs(e / tb - 1.0) <= 0.03
        out.append(((w, c), ok))
    return (tb, out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=150)
    args = ap.parse_args()
    rows = []
    with mp.Pool(8) as pool:
        for r in pool.imap(one, _cache.keys()[:args.limit]):
            if r:
                rows.append(r)
    print("%d windows\n" % len(rows))

    truths = np.array([r[0] for r in rows])
    print("the corpus's own tempo distribution:")
    for (a, b) in BINS:
        n = int(((truths >= a) & (truths < b)).sum())
        print("   %3d-%3d : %3d windows" % (a, b, n))
    print("   below %g : %3d windows\n"
          % (LO, int((truths < LO).sum())))

    print("%-18s %-10s %s" % ("config", "overall",
                               "  ".join("%3d-%3d" % (a, b) for (a, b) in BINS)))
    for (w, c) in CONFIGS:
        hits = {k: [0, 0] for k in range(len(BINS))}
        tot = [0, 0]
        for (tb, out) in rows:
            for (key, ok) in out:
                if key != (w, c):
                    continue
                tot[1] += 1
                tot[0] += int(ok)
                for i, (a, b) in enumerate(BINS):
                    if a <= tb < b:
                        hits[i][1] += 1
                        hits[i][0] += int(ok)
                        break
        print("w=%.2f C=%-6.0f %3d/%3d (%3.0f%%)   %s"
              % (w, c, tot[0], tot[1], 100.0 * tot[0] / max(1, tot[1]),
                 "  ".join("%3d/%2d" % (hits[i][0], hits[i][1])
                           for i in range(len(BINS)))))
    return 0


if __name__ == "__main__":
    sys.exit(main())

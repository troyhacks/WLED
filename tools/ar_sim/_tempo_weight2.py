#!/usr/bin/env python3
"""THE WEIGHT AT THE CORPUS CENTRE - the corpus half only.

_tempo_weight.py printed the synthetic half and then died before the
corpus half.  This is the corpus half alone, at C = 125 (the corpus's own
centre), over the same weights, so the two halves can be read against
each other.

    python tools/ar_sim/_tempo_weight2.py --limit 120
"""

import argparse
import multiprocessing as mp
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402
from _tempo_range import configure, oracle         # noqa: E402

LO, HI, CENTRE, SIGMA = 118.0, 176.0, 125.0, 0.35
WEIGHTS = [0.0, 0.2, 0.3, 0.4, 0.5, 0.6]


def cfg(w):
    configure(lo=LO, hi=HI, prior_weight=w, prior_bpm=CENTRE, prior_sigma=SIGMA)


def corpus_one(key):
    o = oracle(key)
    if not o:
        return None
    tb = o[0]
    x = _cache.load(key)
    if x is None:
        return None
    odf = B.firmware_odf(x, 80.0, 320.0)
    out = []
    for w in WEIGHTS:
        cfg(w)
        t = P.Tracker()
        t.run(odf)
        e = t.bpm
        if e <= 0:
            cls = "none"
        elif abs(e / tb - 1.0) <= 0.03:
            cls = "right"
        elif abs(e / (2.0 * tb) - 1.0) <= 0.03:
            cls = "fast"
        elif abs(e / (0.5 * tb) - 1.0) <= 0.03:
            cls = "half"
        elif abs(e / tb - 4.0 / 3.0) < 0.06:
            cls = "4:3"
        elif abs(e / tb - 1.25) < 0.04:
            cls = "5:4"
        elif abs(e / tb - 1.5) < 0.06:
            cls = "3:2"
        else:
            cls = "other"
        out.append((w, cls))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=120)
    args = ap.parse_args()

    kinds = {w: {} for w in WEIGHTS}
    with mp.Pool(8) as pool:
        for res in pool.imap(corpus_one, _cache.keys()[:args.limit]):
            if res is None:
                continue
            for (w, cls) in res:
                d = kinds[w]
                d[cls] = d.get(cls, 0) + 1
    print("\n=== REAL CORPUS (house), catalogue-fast level, band 118-176, C=125 ===")
    for w in WEIGHTS:
        d = kinds[w]
        n = sum(d.values())
        r = d.get("right", 0)
        parts = " ".join("%s %d" % (k, v) for k, v in
                         sorted(d.items(), key=lambda t: -t[1]))
        print("w=%.2f  %3d/%3d (%3.0f%%)  %s"
              % (w, r, n, 100.0 * r / max(1, n), parts))
    return 0


if __name__ == "__main__":
    sys.exit(main())

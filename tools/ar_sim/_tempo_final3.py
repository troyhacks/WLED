#!/usr/bin/env python3
"""THE LAST THREE MEASUREMENTS, in one pass.

1. THE HIT RATE BINNED BY THE TRUE TEMPO, on both populations.  The corpus
   is concentrated at 120-135; the synthetic set is spread evenly over
   118-176.  A single hit rate hides WHICH TEMPOS a configuration loses.

2. THE OUT-OF-BAND WINDOWS: what does the tracker publish on music the band
   cannot explain?  Specifically its SCORE and whether it is VALID - because
   the info row's "nothing in the band" case only fires when nothing inside
   the band reaches MIN_SCORE.

3. THE "OTHER" CLASS: the in-band misses that are not an alias.  What are
   they?

    python tools/ar_sim/_tempo_final3.py --limit 150
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
CONFIGS = [(0.20, 118.0), (0.30, 118.0), (0.60, 125.0)]
BINS = [(118, 125), (125, 132), (132, 140), (140, 150), (150, 176)]
MIN_SCORE = 0.10


def cfg(w, c):
    configure(lo=LO, hi=HI, prior_weight=w, prior_bpm=c, prior_sigma=SIGMA)


def cls_of(e, tb):
    if e <= 0:
        return "none"
    if abs(e / tb - 1.0) <= 0.03:
        return "right"
    if abs(e / tb - 2.0) <= 0.03:
        return "fast"
    if abs(e / tb - 0.5) <= 0.03:
        return "half"
    if abs(e / tb - 4.0 / 3.0) < 0.06:
        return "4:3"
    if abs(e / tb - 1.25) < 0.04:
        return "5:4"
    if abs(e / tb - 1.5) < 0.06:
        return "3:2"
    return "other"


def corpus_one(key):
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
        cfg(w, c)
        t = P.Tracker()
        t.run(odf)
        out.append(dict(key=(w, c), tb=tb, e=float(t.bpm), sc=float(t.score),
                        cls=cls_of(t.bpm, tb)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=150)
    args = ap.parse_args()

    rows = []
    with mp.Pool(4) as pool:
        for r in pool.imap(corpus_one, _cache.keys()[:args.limit]):
            if r:
                rows.extend(r)
    print("%d corpus windows\n" % len(rows))

    truths = np.array([r["tb"] for r in rows])
    print("the corpus's tempo distribution (n=%d):" % len(rows))
    for (a, b) in BINS:
        n = int(((truths >= a) & (truths < b)).sum())
        print("   %3d-%3d : %3d windows" % (a, b, n))
    print("   below %g : %3d windows\n" % (LO, int((truths < LO).sum())))

    print("1. THE HIT RATE BINNED BY THE TRUE TEMPO")
    print("%-14s %-14s %s" % ("config", "overall",
                               "  ".join("%5d-%3d" % (a, b) for (a, b) in BINS)))
    for (w, c) in CONFIGS:
        hits = {i: [0, 0] for i in range(len(BINS))}
        tot = [0, 0]
        for r in rows:
            if r["key"] != (w, c):
                continue
            tot[1] += 1
            tot[0] += int(r["cls"] == "right")
            for i, (a, b) in enumerate(BINS):
                if a <= r["tb"] < b:
                    hits[i][1] += 1
                    hits[i][0] += int(r["cls"] == "right")
                    break
        print("w=%.2f C=%-5.0f %3d/%3d (%3.0f%%)   %s"
              % (w, c, tot[0], tot[1], 100.0 * tot[0] / max(1, tot[1]),
                 "  ".join("%3d/%3d" % (hits[i][0], hits[i][1])
                           for i in range(len(BINS)))))
    print()

    print("2. THE OUT-OF-BAND WINDOWS: what does the tracker publish?")
    for (w, c) in CONFIGS:
        outs = [r for r in rows
                if r["key"] == (w, c) and not (LO <= r["tb"] <= HI)]
        if not outs:
            continue
        print("   w=%.2f C=%-5.0f  n=%d" % (w, c, len(outs)))
        for r in sorted(outs, key=lambda r: r["tb"]):
            print("      truth %7.2f -> est %7.2f  score %.3f  %s"
                  % (r["tb"], r["e"], r["sc"],
                     "VALID" if r["sc"] >= MIN_SCORE else "invalid"))
        print("      -> %d of %d are published VALID (score >= %.2f)"
              % (sum(1 for r in outs if r["sc"] >= MIN_SCORE), len(outs),
                 MIN_SCORE))
    print()

    print("3. THE IN-BAND MISSES THAT ARE NOT AN ALIAS")
    for (w, c) in CONFIGS:
        miss = [r for r in rows if r["key"] == (w, c)
                and LO <= r["tb"] <= HI and r["cls"] not in ("right",)]
        other = [r for r in miss if r["cls"] == "other"]
        print("   w=%.2f C=%-5.0f  %d misses, %d of them 'other'"
              % (w, c, len(miss), len(other)))
        for r in sorted(other, key=lambda r: r["tb"]):
            print("      truth %7.2f -> est %7.2f  ratio %.3f  score %.3f"
                  % (r["tb"], r["e"], r["e"] / r["tb"], r["sc"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""THE WEIGHT, DECISIVELY: C = 125 (the corpus's own centre), band 118-176.

The prior sweep (_tempo_prior2.py) varies the centre and the weight at once,
and most of its trials centre at 118 or 112.  This one fixes the centre at
125 - the corpus's mean tempo is 125.85 - and walks the weight, on the REAL
corpus and on the synthetic set whose truths are spread over 118-176.

The point is to find the weight that keeps the house corpus AND the fast end
of the band, and to report the bracketing measurements so the choice can be
argued from the numbers.

    python tools/ar_sim/_tempo_weight.py --limit 120
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
from _tempo_band_synth import fw_odf, classify     # noqa: E402
from _tempo2_sim import STYLES, make_track, _seed  # noqa: E402

LO, HI, CENTRE, SIGMA = 118.0, 176.0, 125.0, 0.35
WEIGHTS = [0.0, 0.2, 0.3, 0.4, 0.5, 0.6]
BPMS = [118, 120, 124, 128, 132, 135, 140, 145, 150, 155, 160, 165, 170,
        174, 176]


def cfg(w):
    configure(lo=LO, hi=HI, prior_weight=w, prior_bpm=CENTRE, prior_sigma=SIGMA)


def synth_one(job):
    (style, bpm) = job
    x = make_track(bpm, seconds=16.0, style=style, seed=_seed(style, bpm))
    odf = fw_odf(x)
    if len(odf) < 200:
        return None
    out = []
    for w in WEIGHTS:
        cfg(w)
        t = P.Tracker()
        t.run(odf)
        r = t.bpm / bpm if t.bpm > 0 else 0.0
        out.append((w, classify(r)))
    return out


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


def report(title, kinds):
    print("\n=== %s ===" % title)
    for w in WEIGHTS:
        d = kinds.get(w, {})
        n = sum(d.values())
        r = d.get("right", 0)
        parts = " ".join("%s %d" % (k, v) for k, v in
                         sorted(d.items(), key=lambda t: -t[1]))
        print("w=%.2f  %3d/%3d (%3.0f%%)  %s"
              % (w, r, n, 100.0 * r / max(1, n), parts))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=120)
    args = ap.parse_args()

    kinds = {w: {} for w in WEIGHTS}
    jobs = [(s, b) for s in STYLES for b in BPMS]
    with mp.Pool(4) as pool:
        for res in pool.imap(synth_one, jobs):
            if res is None:
                continue
            for (w, cls) in res:
                d = kinds[w]
                d[cls] = d.get(cls, 0) + 1
    report("SYNTHETIC, known truths 118-176, band 118-176, C=125", kinds)

    kinds2 = {w: {} for w in WEIGHTS}
    with mp.Pool(4) as pool:
        for res in pool.imap(corpus_one, _cache.keys()[:args.limit]):
            if res is None:
                continue
            for (w, cls) in res:
                d = kinds2[w]
                d[cls] = d.get(cls, 0) + 1
    report("REAL CORPUS (house), catalogue-fast level, band 118-176, C=125",
           kinds2)
    return 0


if __name__ == "__main__":
    sys.exit(main())

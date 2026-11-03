#!/usr/bin/env python3
"""THE PRIOR, PROPERLY: where it goes and how strong it should be.

WHAT IS KNOWN
-------------
On the REAL corpus (house music) the prior is the whole answer: switch it off
and the tracker collapses into the 4:3 alias on half the windows.  But the
corpus is house music, so a prior CENTRED AT 125 is not a general prior - it
is the corpus's own answer, written down.

On tracks whose tempo is KNOWN and spread across the band, the same prior is
a liability: it taxes the fast end of the band, and the fast end is where a
repertoire that reaches 170 BPM lives.

THE QUESTION
------------
The alias families a band cannot exclude are all FASTER than the truth (2:1,
3:2, 4:3, 5:4 all sit above it).  So the prior that breaks them has to be
MONOTONE INCREASING over the band - and a log-normal is monotone increasing
over a band only when its centre is at or BELOW the band's bottom.  A
centre in the middle of the band is a SYMMETRIC prior, and a symmetric prior
prefers the alias for every track below its centre.

So the candidate design is: centre the prior at the band's BOTTOM (or below)
and choose the weight by measurement.  This sweeps the centre and the weight
on both corpora at once.

    python tools/ar_sim/_tempo_prior2.py --limit 120
"""

import argparse
import math
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

LO, HI = 118.0, 176.0

TRIALS = [
    ("none        ", 0.0, 147.0),
    ("w0.20 @125  ", 0.2, 125.0),
    ("w0.60 @125  ", 0.6, 125.0),
    ("w0.20 @118  ", 0.2, 118.0),
    ("w0.30 @118  ", 0.3, 118.0),
    ("w0.45 @118  ", 0.45, 118.0),
    ("w0.60 @118  ", 0.6, 118.0),
    ("w0.90 @118  ", 0.9, 118.0),
    ("w0.30 @112  ", 0.3, 112.0),
    ("w0.60 @112  ", 0.6, 112.0),
    ("w0.45 @105  ", 0.45, 105.0),
]

BPMS = [118, 120, 124, 128, 132, 135, 140, 145, 150, 155, 160, 165, 170,
        174, 176]


def cfg(w, c):
    configure(lo=LO, hi=HI, prior_weight=w, prior_bpm=c, prior_sigma=0.35)


def synth_one(job):
    (style, bpm) = job
    x = make_track(bpm, seconds=16.0, style=style, seed=_seed(style, bpm))
    odf = fw_odf(x)
    if len(odf) < 200:
        return None
    out = []
    for (label, w, c) in TRIALS:
        cfg(w, c)
        t = P.Tracker()
        t.run(odf)
        r = t.bpm / bpm if t.bpm > 0 else 0.0
        out.append((label, classify(r)))
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
    for (label, w, c) in TRIALS:
        cfg(w, c)
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
        elif abs(e / tb - 1.5) < 0.06:
            cls = "3:2"
        else:
            cls = "other"
        out.append((label, cls))
    return out


def report(title, kinds):
    print("\n=== %s ===" % title)
    for (label, w, c) in TRIALS:
        d = kinds.get(label, {})
        n = sum(d.values())
        r = d.get("right", 0)
        parts = " ".join("%s %d" % (k, v) for k, v in
                         sorted(d.items(), key=lambda t: -t[1]))
        print("%s  w=%.2f C=%.0f  %3d/%3d (%3.0f%%)  %s"
              % (label, w, c, r, n, 100.0 * r / max(1, n), parts))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=120)
    args = ap.parse_args()

    # ---- the synthetic set: known truths, spread across the band ----
    jobs = [(s, b) for s in STYLES for b in BPMS]
    kinds = {t[0]: {} for t in TRIALS}
    with mp.Pool(8) as pool:
        for res in pool.imap(synth_one, jobs):
            if res is None:
                continue
            for (label, cls) in res:
                d = kinds[label]
                d[cls] = d.get(cls, 0) + 1
    report("SYNTHETIC, known truths 118-176, band 118-176", kinds)

    # ---- the real corpus: house music ----
    kinds2 = {t[0]: {} for t in TRIALS}
    with mp.Pool(8) as pool:
        for res in pool.imap(corpus_one, _cache.keys()[:args.limit]):
            if res is None:
                continue
            for (label, cls) in res:
                d = kinds2[label]
                d[cls] = d.get(cls, 0) + 1
    report("REAL CORPUS (house), catalogue-fast level, band 118-176", kinds2)
    return 0


if __name__ == "__main__":
    sys.exit(main())

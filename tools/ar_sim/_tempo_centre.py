#!/usr/bin/env python3
"""THE CENTRE, at the club band's bottom, with the in-band population
separated from the out-of-band one.

The prior's centre is a property of the band: the condition for the prior
to break the r-family over the WHOLE band is C <= Bmin*sqrt(r).  For the
club band that is C <= 118*sqrt(1.25) = 131.93.  So every centre from 118
to 131 is LEGAL.  This measures which one is best, at the weights that are
legal for it, and it separates the windows the band can explain from the
ones it deliberately excludes.

    python tools/ar_sim/_tempo_centre.py --limit 150
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
from _tempo_band_synth import fw_odf               # noqa: E402
from _tempo2_sim import STYLES, make_track, _seed  # noqa: E402

LO, HI, SIGMA = 118.0, 176.0, 0.35
CENTRES = [118.0, 121.0, 125.0, 128.0, 131.0]
WEIGHTS = [0.15, 0.20, 0.25, 0.30, 0.40]
BPMS = [118, 120, 124, 128, 132, 135, 140, 145, 150, 155, 160, 165, 170,
        174, 176]


def cfg(w, c):
    configure(lo=LO, hi=HI, prior_weight=w, prior_bpm=c, prior_sigma=SIGMA)


def cls_of(e, tb):
    if e <= 0:
        return "none"
    if abs(e / tb - 1.0) <= 0.03:
        return "right"
    if abs(e / (2.0 * tb) - 1.0) <= 0.03:
        return "fast"
    if abs(e / (0.5 * tb) - 1.0) <= 0.03:
        return "half"
    if abs(e / tb - 4.0 / 3.0) < 0.06:
        return "4:3"
    if abs(e / tb - 1.25) < 0.04:
        return "5:4"
    if abs(e / tb - 1.5) < 0.06:
        cls = "3:2"
    else:
        cls = "other"
    return cls


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
    for c in CENTRES:
        for w in WEIGHTS:
            cfg(w, c)
            t = P.Tracker()
            t.run(odf)
            out.append(((c, w), cls_of(t.bpm, tb)))
    return (tb, out)


def synth_one(job):
    (style, bpm) = job
    x = make_track(bpm, seconds=16.0, style=style, seed=_seed(style, bpm))
    odf = fw_odf(x)
    if len(odf) < 200:
        return None
    out = []
    for c in CENTRES:
        for w in WEIGHTS:
            cfg(w, c)
            t = P.Tracker()
            t.run(odf)
            out.append(((c, w), cls_of(t.bpm, float(bpm))))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=150)
    args = ap.parse_args()

    rows = []
    with mp.Pool(8) as pool:
        for r in pool.imap(corpus_one, _cache.keys()[:args.limit]):
            if r:
                rows.append(r)
    truths = np.array([r[0] for r in rows])
    inband = (truths >= LO - 1e-6) & (truths <= HI + 1e-6)
    print("REAL CORPUS: %d windows, %d in 118-176, %d below\n"
          % (len(rows), int(inband.sum()), int((~inband).sum())))

    tab = {}
    for c in CENTRES:
        for w in WEIGHTS:
            key = (c, w)
            ins, outs = [], []
            k = 0
            for (tb, out) in rows:
                for (kk, cl) in out:
                    if kk == key:
                        (ins if inband[k] else outs).append(cl)
                k += 1
            tab[key] = (ins, outs)
    print("%-8s %-6s %-22s %-14s" % ("C", "w", "IN the band", "OUT the band"))
    for c in CENTRES:
        for w in WEIGHTS:
            ins, outs = tab[(c, w)]
            r = ins.count("right")
            print("%-8.0f %-6.2f %3d/%3d (%3.0f%%) alias %3d   right %2d/%3d"
                  % (c, w, r, len(ins), 100.0 * r / max(1, len(ins)),
                     sum(1 for x in ins if x in ("4:3", "5:4", "3:2", "fast",
                                                "half")),
                     outs.count("right"), len(outs)))
    print()
    srows = []
    jobs = [(s, b) for s in STYLES for b in BPMS]
    with mp.Pool(8) as pool:
        for res in pool.imap(synth_one, jobs):
            if res:
                srows.append(res)
    print("SYNTHETIC: %d tracks, truths spread over 118-176\n" % len(srows))
    for c in CENTRES:
        for w in WEIGHTS:
            allc = []
            for res in srows:
                for (kk, cl) in res:
                    if kk == (c, w):
                        allc.append(cl)
            n = len(allc)
            print("%-8.0f %-6.2f %3d/%3d (%3.0f%%)  4:3 %3d  5:4 %3d  3:2 %3d"
                  % (c, w, allc.count("right"), n,
                     100.0 * allc.count("right") / max(1, n),
                     allc.count("4:3"), allc.count("5:4"), allc.count("3:2")))
    return 0


if __name__ == "__main__":
    sys.exit(main())

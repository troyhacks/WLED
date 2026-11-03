#!/usr/bin/env python3
"""THE FINE SWEEP AT THE BAND'S BOTTOM, on BOTH corpora.

The prior's centre at C = Bmin makes the penalty monotone increasing over
the band, and the differential the prior produces over a truth/alias pair
is then largest at the band's bottom - which is exactly where the alias
families the band cannot exclude (4:3, 5:4) live.  So the binding point
is t = Bmin and the binding family is 5:4.

    d_5:4(t=118, C=118) = w * k^2 = w * 0.406475
    three times the measured median 5:4 evidence gap (3 * 0.0266):
        w = 0.0798 / 0.406475 = 0.196  ->  w = 0.20

This measures that prediction on both corpora, with the weights around it.

    python tools/ar_sim/_tempo_w118.py --limit 150
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

LO, HI, C, SIGMA = 118.0, 176.0, 118.0, 0.35
WEIGHTS = [0.00, 0.15, 0.20, 0.25, 0.30, 0.35, 0.40]
BPMS = [118, 120, 124, 128, 132, 135, 140, 145, 150, 155, 160, 165, 170,
        174, 176]


def cfg(w):
    configure(lo=LO, hi=HI, prior_weight=w, prior_bpm=C, prior_sigma=SIGMA)


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
    for w in WEIGHTS:
        cfg(w)
        t = P.Tracker()
        t.run(odf)
        out.append((w, cls_of(t.bpm, tb)))
    return (tb, out)


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
        out.append((w, cls_of(t.bpm, float(bpm))))
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
    print("%-7s %-24s %-16s" % ("w", "IN the band", "OUT the band"))
    for w in WEIGHTS:
        ins, outs = [], []
        k = 0
        for (tb, out) in rows:
            for (ww, cl) in out:
                if ww == w:
                    (ins if inband[k] else outs).append(cl)
            k += 1
        print("%-7.2f %3d/%3d (%3.0f%%) alias %3d   right %2d/%3d"
              % (w, ins.count("right"), len(ins),
                 100.0 * ins.count("right") / max(1, len(ins)),
                 sum(1 for x in ins if x in ("4:3", "5:4", "3:2", "fast", "half")),
                 outs.count("right"), len(outs)))
    print()
    srows = []
    jobs = [(s, b) for s in STYLES for b in BPMS]
    with mp.Pool(8) as pool:
        for res in pool.imap(synth_one, jobs):
            if res:
                srows.append(res)
    print("SYNTHETIC: %d tracks, truths spread over 118-176, C=%g\n"
          % (len(srows), C))
    for w in WEIGHTS:
        allc = []
        for res in srows:
            for (ww, cl) in res:
                if ww == w:
                    allc.append(cl)
        n = len(allc)
        print("%-7.2f %3d/%3d (%3.0f%%)  4:3 %3d  5:4 %3d  3:2 %3d"
              % (w, allc.count("right"), n,
                 100.0 * allc.count("right") / max(1, n),
                 allc.count("4:3"), allc.count("5:4"), allc.count("3:2")))
    return 0


if __name__ == "__main__":
    sys.exit(main())

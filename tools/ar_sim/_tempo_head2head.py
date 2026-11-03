#!/usr/bin/env python3
"""HEAD TO HEAD: the shipped configuration against the proposed ones, on
the SAME windows, scored the SAME way.

The corpus number in every earlier table is capped: the club band excludes
the windows whose truth is below 118, and those can never be read right.
So this reports the in-band population and the out-of-band population
SEPARATELY, and it puts the shipped configuration (band 90-175, prior
125/0.60) in the same table as the proposals.

    python tools/ar_sim/_tempo_head2head.py --limit 150
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

SIGMA = 0.35
CONFIGS = [
    ("shipped  90-175  C125 w0.60", 90.0, 175.0, 125.0, 0.60),
    ("         90-175  C118 w0.30", 90.0, 175.0, 118.0, 0.30),
    ("         90-175  C118 w0.20", 90.0, 175.0, 118.0, 0.20),
    ("club     118-176 C118 w0.30", 118.0, 176.0, 118.0, 0.30),
    ("club     118-176 C118 w0.20", 118.0, 176.0, 118.0, 0.20),
    ("club     118-176 C125 w0.30", 118.0, 176.0, 125.0, 0.30),
    ("club     118-176 C125 w0.20", 118.0, 176.0, 125.0, 0.20),
]
CLUB_LO, CLUB_HI = 118.0, 176.0
BPMS = [118, 120, 124, 128, 132, 135, 140, 145, 150, 155, 160, 165, 170,
        174, 176]


def cfg(lo, hi, w, c):
    configure(lo=lo, hi=hi, prior_weight=w, prior_bpm=c, prior_sigma=SIGMA)


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
    for (label, lo, hi, w, c) in CONFIGS:
        cfg(lo, hi, w, c)
        t = P.Tracker()
        t.run(odf)
        out.append((label, cls_of(t.bpm, tb), float(t.bpm)))
    return (tb, out)


def synth_one(job):
    (style, bpm) = job
    x = make_track(bpm, seconds=16.0, style=style, seed=_seed(style, bpm))
    odf = fw_odf(x)
    if len(odf) < 200:
        return None
    out = []
    for (label, lo, hi, w, c) in CONFIGS:
        cfg(lo, hi, w, c)
        t = P.Tracker()
        t.run(odf)
        out.append((label, cls_of(t.bpm, float(bpm)), float(t.bpm)))
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
    inband = (truths >= CLUB_LO - 1e-6) & (truths <= CLUB_HI + 1e-6)
    print("REAL CORPUS: %d windows, %d with the truth inside 118-176, %d below\n"
          % (len(rows), int(inband.sum()), int((~inband).sum())))

    for (label, lo, hi, w, c) in CONFIGS:
        ins, outs = [], []
        for (tb, out) in rows:
            for (l, cl, e) in out:
                if l != label:
                    continue
                (ins if inband[list(rows).index((tb, out))] else outs).append(cl)
        # the index trick above is fragile; recompute cleanly
        ins, outs = [], []
        k = 0
        for (tb, out) in rows:
            for (l, cl, e) in out:
                if l != label:
                    continue
                (ins if inband[k] else outs).append(cl)
            k += 1
        n = len(ins)
        r = ins.count("right")
        alias = sum(1 for x in ins if x in ("4:3", "5:4", "3:2", "fast", "half"))
        print("%-30s IN %3d/%3d (%3.0f%%)  alias %3d   OUT right %2d/%3d"
              % (label, r, n, 100.0 * r / max(1, n), alias,
                 outs.count("right"), len(outs)))

    print()
    srows = []
    jobs = [(s, b) for s in STYLES for b in BPMS]
    with mp.Pool(8) as pool:
        for res in pool.imap(synth_one, jobs):
            if res:
                srows.append(res)
    print("SYNTHETIC: %d tracks, truths spread over 118-176\n" % len(srows))
    for (label, lo, hi, w, c) in CONFIGS:
        allc = []
        for res in srows:
            for (l, cl, e) in res:
                if l == label:
                    allc.append(cl)
        n = len(allc)
        r = allc.count("right")
        print("%-30s %3d/%3d (%3.0f%%)  4:3 %3d  5:4 %3d  3:2 %3d"
              % (label, r, n, 100.0 * r / max(1, n), allc.count("4:3"),
                 allc.count("5:4"), allc.count("3:2")))
    return 0


if __name__ == "__main__":
    sys.exit(main())

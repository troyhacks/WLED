#!/usr/bin/env python3
"""THE FINAL COMPARISON: the shipped configuration against the proposed one.

THE QUESTION
------------
The shipped tracker is  90-175 BPM  +  a log-normal prior at 125 (weight
0.6).  The proposal is  118-176 BPM  +  the same prior at a lower weight,
because:

  * 118-176 has a margin g = +30 (90-175 has +2.5) and is 3:2-free
  * a strong prior centred at 125 is a HOUSE-MUSIC assumption; measured on
    tracks whose tempo is KNOWN it costs 24 points inside 118-176

and the critique's blocker says the default must not sit at the fragile end
of the margin scale.

THE MEASUREMENT
---------------
On the REAL corpus (100 house tracks x 3 windows), against the librosa
oracle, scored at the CATALOGUE-FAST level: right means the estimate
matches the oracle at the oracle's level, at its double, or at its half.

    python tools/ar_sim/_tempo_final.py --limit 120
"""

import argparse
import multiprocessing as mp
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402
from _tempo_range import configure, oracle         # noqa: E402

CONFIGS = [
    ("90-175  w0.6  SHIPPED", 90.0, 175.0, 0.6),
    ("90-175  w0.2",          90.0, 175.0, 0.2),
    ("118-176 w0.6",          118.0, 176.0, 0.6),
    ("118-176 w0.2",          118.0, 176.0, 0.2),
    ("118-176 w0.3",          118.0, 176.0, 0.3),
    ("118-176 w0.0",          118.0, 176.0, 0.0),
    ("70-138  w0.2",          70.0, 138.0, 0.2),
]


def one(key):
    o = oracle(key)
    if not o:
        return None
    tb = o[0]
    x = _cache.load(key)
    if x is None:
        return None
    odf = B.firmware_odf(x, 80.0, 320.0)
    out = []
    for (label, lo, hi, pw) in CONFIGS:
        configure(lo=lo, hi=hi, prior_weight=pw)
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
        out.append((label, cls, e, tb))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=120)
    args = ap.parse_args()
    keys = _cache.keys()[:args.limit]
    t0 = time.time()
    res = {c[0]: {} for c in CONFIGS}
    detail = {c[0]: [] for c in CONFIGS}
    with mp.Pool(8) as pool:
        for i, r in enumerate(pool.imap(one, keys)):
            if r is None:
                continue
            for (label, cls, e, tb) in r:
                res[label][cls] = res[label].get(cls, 0) + 1
                if cls not in ("right",):
                    detail[label].append((cls, e, tb))
            if (i + 1) % 20 == 0:
                print("  %d/%d (%.0f s)" % (i + 1, len(keys),
                                            time.time() - t0), flush=True)
    print()
    for (label, lo, hi, pw) in CONFIGS:
        d = res[label]
        n = sum(d.values())
        parts = " ".join("%s %d" % (k, v) for k, v in sorted(d.items(),
                                                             key=lambda t: -t[1]))
        g = lo - hi / 2.0
        print("%-24s g=%+5.1f n=%3d  %s" % (label, g, n, parts))
    print()
    for (label, lo, hi, pw) in CONFIGS:
        if label != "118-176 w0.2":
            continue
        print("the misses of %s:" % label)
        for (cls, e, tb) in sorted(detail[label])[:40]:
            print("   %-6s est %7.2f  oracle %7.2f  ratio %.3f"
                  % (cls, e, tb, e / tb if tb else 0.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())

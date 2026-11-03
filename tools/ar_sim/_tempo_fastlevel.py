#!/usr/bin/env python3
"""THE CATALOGUE-FAST LEVEL: the metric a DJ tool is judged by.

THE PROBLEM WITH THE RAW METRIC
-------------------------------
The corpus's oracle is librosa's tempo, and librosa sometimes reports the
HALF-TIME of a house track: 14 of the 300 windows sit between 64 and 90
BPM, and every one of those doubles into 118-176.  A house track whose
kick lands every 0.8 s is catalogued at 128 BPM, not at 64: the DJ counts
the kick, not the half-time.

So comparing a tracker's BPM to librosa's BPM marks a CORRECT fast-level
answer as wrong, and the sizes line up: the corpus's low tail is 64.66,
89.29, 108.17, 114.80, 117.19 - and their doubles are 129.3, 178.6, 216,
229.6, 234.4.

THE METRIC
----------
An estimate is RIGHT if it matches the oracle at the oracle's level, at
twice it, or at half it - within 3 %.  And the two kinds of "right" are
counted separately, because they are not the same achievement:

    fast   the estimate is the DOUBLE of the oracle: the tracker's right
           and the oracle's half-time.  This is what a DJ wants.
    half   the estimate is the HALF of the oracle: the tracker picked the
           slow reading.  This is the failure mode a range must prevent.

    python tools/ar_sim/_tempo_fastlevel.py --limit 150
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402
from _tempo_range import configure, oracle         # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=150)
    args = ap.parse_args()

    bands = [
        ("90-175 +prior", 90.0, 175.0, 0.6),
        ("90-175 noprior", 90.0, 175.0, 0.0),
        ("118-176 +prior", 118.0, 176.0, 0.6),
        ("118-176 noprior", 118.0, 176.0, 0.0),
        ("118-176 pw0.2", 118.0, 176.0, 0.2),
        ("120-176 +prior", 120.0, 176.0, 0.6),
        ("70-140 +prior", 70.0, 140.0, 0.6),
        ("70-176 +prior", 70.0, 176.0, 0.6),
    ]

    rows = []
    for (label, lo, hi, pw) in bands:
        n = 0
        right = fast = half = other = 0
        for k in _cache.keys()[:args.limit]:
            o = oracle(k)
            if not o:
                continue
            tb = o[0]
            x = _cache.load(k)
            if x is None:
                continue
            odf = B.firmware_odf(x, 80.0, 320.0)
            configure(lo=lo, hi=hi, prior_weight=pw)
            t = P.Tracker()
            t.run(odf)
            n += 1
            if t.bpm <= 0:
                other += 1
                continue
            if abs(t.bpm / tb - 1.0) <= 0.03:
                right += 1
            elif abs(t.bpm / (2.0 * tb) - 1.0) <= 0.03:
                fast += 1
            elif abs(t.bpm / (0.5 * tb) - 1.0) <= 0.03:
                half += 1
            else:
                other += 1
        rows.append("%-18s n=%3d  right %3d (%3.0f%%)  fast %3d  half %3d  "
                    "other %3d"
                    % (label, n, right, 100.0 * right / n, fast, half, other))

    print("\n".join(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())

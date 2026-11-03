#!/usr/bin/env python3
"""THE 4:3 ALIAS, AND THE ODF BAND THAT CAUSES IT.

WHAT IS KNOWN
-------------
On the real corpus, the shipped tracker reads 4:3 of the truth on 56 of
100 windows when the log-normal prior is switched off.  4:3 is not an
octave and not a 3:2 - it is the NEXT alias, and it is the only error
class a narrow band leaves, because a band [Bmin,Bmax] with
Bmax < 1.5*Bmin contains no octave pair and no 3:2 pair, but it does
contain 4:3 pairs unless Bmax < 4/3*Bmin - which is too narrow for a DJ.

So a range cannot exclude the 4:3 by itself.  Something has to exclude
it, and the candidate is the ODF's BAND: the shipped band is 80-320 Hz,
and a kick's fundamental is 45-70 Hz.  The band therefore EXCLUDES the
very onset that defines the beat and keeps the snare and the hats - and
the hat grid is a subdivision of the beat, so the alias that wins is a
subdivision alias.

THE MEASUREMENT
---------------
For each ODF band, and with the prior off and on, on the REAL corpus,
scored against the oracle at the catalogue-fast level:

    right   matches the oracle (at its level, or its double, or its half)
    4:3     the estimate is 4/3 of the truth
    other   everything else

    python tools/ar_sim/_tempo_alias.py --limit 150
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


def classify(est, tbpm):
    """(right, class) against the oracle, at the catalogue-fast level."""
    if est <= 0:
        return False, "none"
    for target in (tbpm, 2.0 * tbpm, 0.5 * tbpm):
        if abs(est / target - 1.0) <= 0.03:
            return True, "right"
    r = est / tbpm
    for (target, name) in ((4.0 / 3.0, "4:3"), (3.0 / 4.0, "3:4"),
                           (2.0, "octave"), (0.5, "octave"),
                           (1.5, "3:2"), (0.6667, "3:2")):
        if abs(r - target) < 0.06:
            return False, name
    return False, "other"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=150)
    args = ap.parse_args()

    bands = [(80.0, 320.0), (40.0, 160.0), (30.0, 200.0), (50.0, 250.0),
             (35.0, 300.0)]
    priors = [("prior 0.6", 0.6), ("prior 0.0", 0.0)]

    keys = []
    truths = []
    for k in _cache.keys()[:args.limit]:
        o = oracle(k)
        if not o:
            continue
        x = _cache.load(k)
        if x is None:
            continue
        keys.append(k)
        truths.append(o[0])

    for (lo, hi) in bands:
        odfs = {}
        for k in keys:
            x = _cache.load(k)
            odfs[k] = B.firmware_odf(x, lo, hi)
        for (plabel, pw) in priors:
            kinds = {}
            for i, k in enumerate(keys):
                configure(lo=118.0, hi=176.0, prior_weight=pw)
                t = P.Tracker()
                t.run(odfs[k])
                _r, c = classify(t.bpm, truths[i])
                kinds[c] = kinds.get(c, 0) + 1
            n = sum(kinds.values())
            parts = " ".join("%s %d" % (kk, vv)
                             for kk, vv in sorted(kinds.items(),
                                                  key=lambda t: -t[1]))
            print("odf %3d-%3d Hz  %s  n=%3d   %s"
                  % (lo, hi, plabel, n, parts))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""What does the downbeat threshold of 5.0 actually mean?

The comment on AR_TEMPO_DOWNBEAT_T claims the threshold sits above the
null's 95th percentile.  It does not: the null's 95th percentile is
5.86.  This measures the null's DISTRIBUTION (every roll, pooled) and
says exactly where 5.0 sits in it - and how often a REAL window would
cross it.

    python tools/ar_sim/_fw_downnull.py --windows 60
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_down as D                               # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=60)
    ap.add_argument("--half-ms", type=float, default=50.0)
    ap.add_argument("--rolls", type=int, default=12)
    args = ap.parse_args()

    half = int(round((args.half_ms * 0.001) * 93.75))
    rolls = []
    means = []
    real = []
    above = 0
    rolls16 = []
    real16 = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, 90.0, 320.0)
        if len(odf) < 1200:
            continue
        t = D.make()
        t.run(odf)
        if len(t.beats) < 32:
            continue
        sal = D.peak_at(odf, t.beats, half)
        if len(sal) < 32:
            continue
        # the real statistic
        r, _ = D.tscore(sal, 4)
        real.append(r)
        # the null: the same beats with the bar's alignment rolled
        rng = np.random.default_rng(1234)
        rr = []
        for _ in range(args.rolls):
            shift = int(rng.integers(1, 4))
            rr.append(D.tscore(np.roll(sal, shift), 4)[0])
        rolls.extend(rr)
        means.append(float(np.mean(rr)))
        if r > np.mean(rr):
            above += 1
        # the measure: sixteen positions
        r16, _ = D.tscore(sal, 16)
        real16.append(r16)
        rr16 = []
        rng = np.random.default_rng(1234)
        for _ in range(args.rolls):
            shift = int(rng.integers(1, 16))
            rr16.append(D.tscore(np.roll(sal, shift), 16)[0])
        rolls16.extend(rr16)

    rolls = np.array(rolls)
    means = np.array(means)
    real = np.array(real)
    if len(rolls) == 0:
        print("no windows")
        return

    print("%d windows, %d null rolls" % (len(real), len(rolls)))
    print("the null's distribution:")
    for q in (50, 75, 90, 95, 97.5, 99):
        print("   p%-5s %6.2f" % (q, np.percentile(rolls, q)))
    print("   max    %6.2f" % rolls.max())
    print("the null's per-window MEAN (what the tool reported):")
    for q in (50, 90, 95):
        print("   p%-5s %6.2f" % (q, np.percentile(means, q)))
    print()
    for th in (3.0, 4.0, 4.5, 5.0, 5.5, 6.0, 7.0):
        print("threshold %4.1f: the null passes it %5.2f %% of the rolls"
              "   the REAL statistic passes it %5.1f %% of the windows"
              % (th, 100.0 * (rolls > th).mean(), 100.0 * (real > th).mean()))
    print()
    print("the real statistic: median %6.2f  p90 %6.2f  max %6.2f"
          % (np.median(real), np.percentile(real, 90), real.max()))
    print("the real score is ABOVE the null's mean in %d of %d windows (%.0f %%)"
          % (above, len(real), 100.0 * above / max(1, len(real))))

    r16 = np.array(rolls16)
    if len(r16):
        print()
        print("the MEASURE (sixteen positions): the null's distribution")
        for q in (50, 90, 95):
            print("   p%-5s %6.2f" % (q, np.percentile(r16, q)))
        for th in (3.5, 4.0, 4.5, 5.0):
            print("threshold %4.1f: the null passes it %5.2f %% of the rolls"
                  "   the REAL statistic passes it %5.1f %% of the windows"
                  % (th, 100.0 * (r16 > th).mean(),
                     100.0 * (np.array(real16) > th).mean()))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""THE CONSTANTS - the numbers the firmware's thresholds need.

Three of them:

  the SCORE's floor.  The score is the box's capture of the onset strength,
  over the total, so a phase-uniform signal would score 2*w/64 - the width
  of the box as a fraction of the circle.  That is the floor, and a
  threshold BELOW it can never fire.

  the DOWNBEAT's threshold.  The t-score of the bar's profile, against the
  null - the same beats with the bar's alignment rolled, which is the same
  statistic on data with no bar structure in it.

  the PHASE's bias, with the plateau's centre instead of its leading edge.

    python tools/ar_sim/_fw_consts.py --windows 60
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402
from _fw_down import make, peak_at, profile, tscore, null_score   # noqa: E402
from _diag_phase import kick_onsets                               # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=60)
    ap.add_argument("--half-ms", type=float, default=50.0)
    args = ap.parse_args()

    half = int(round((args.half_ms * 0.001) * P.RATE))
    scores = []
    t4s, n4s, t16s, n16s = [], [], [], []
    biases = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, 90.0, 320.0)
        if len(odf) < 1200:
            continue
        t = make()
        t.run(odf)
        if not t.beats:
            continue
        sc = np.array(t.score_log)
        sc = sc[sc > 0]
        if len(sc):
            scores.append(float(np.median(sc)))
        # the downbeat's statistic
        s = peak_at(odf, t.beats, half)
        if len(s) >= 32:
            t4s.append(tscore(s, 4)[0])
            n4s.append(null_score(s, 4, rolls=16))
            t16s.append(tscore(s, 16)[0])
            n16s.append(null_score(s, 16, rolls=16))
        # the phase's bias against the kick
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        if ons is not None and len(ons):
            b = np.array(t.beats) / P.RATE
            o = np.asarray(ons, dtype=float)
            d = b[:, None] - o[None, :]
            j = np.argmin(np.abs(d), axis=1)
            biases.append(float(np.median(d[np.arange(len(b)), j] * 1000.0)))

    s = np.array(scores)
    print("the SCORE, over %d windows" % len(s))
    print("  p5 %.3f  p10 %.3f  median %.3f  p90 %.3f"
          % (np.percentile(s, 5), np.percentile(s, 10),
             np.median(s), np.percentile(s, 90)))
    print("  the floor (a phase-uniform signal): 2*w/64 = %.3f at 90 BPM, "
          "%.3f at 120 BPM and above"
          % (2.0 * (6.5625 / 62.5 * 64) / 64.0, 2.0 * 8.96 / 64.0))
    print()
    t4 = np.array(t4s); n4 = np.array(n4s)
    t16 = np.array(t16s); n16 = np.array(n16s)
    print("the DOWNBEAT's t-score, and its null")
    print("  the bar   : observed median %.2f  the null's median %.2f  "
          "the null's p95 %.2f" % (np.median(t4), np.median(n4),
                                   np.percentile(n4, 95)))
    print("  the bar   : the observed above the null in %.0f%% of the windows"
          % (100.0 * (t4 > n4).mean()))
    print("  the measure: observed median %.2f  the null's median %.2f  "
          "the null's p95 %.2f" % (np.median(t16), np.median(n16),
                                   np.percentile(n16, 95)))
    print("  the measure: the observed above the null in %.0f%% of the windows"
          % (100.0 * (t16 > n16).mean()))
    print()
    b = np.array(biases)
    print("the PHASE's bias against the kick: median %+.1f ms  mean %+.1f ms  "
          "(%d windows)" % (np.median(b), b.mean(), len(b)))


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""THE WHOLE TRACKER, as the firmware now has it - checked end to end.

This is the twin with the firmware's own settings - the capped kernel, the
plateau's centre, the raised score floor - and it measures the four things
that matter:

  the RATE against librosa's independent beat tracker
  the SCORE's distribution, so the floor is above the noise and below the
  music
  the DJ-style MIX, where the old kernel climbed to the top of the band
  the PHASE, against the kick

    python tools/ar_sim/_fw_check.py --windows 60 --mix 12
"""

import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402

ORACLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_oracle")


def oracle_bpm(key):
    p = os.path.join(ORACLE, key + ".json")
    if not os.path.exists(p):
        return 0.0
    with open(p, encoding="utf-8") as f:
        return float(json.load(f).get("bpm", 0.0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=60)
    ap.add_argument("--mix", type=int, default=12)
    args = ap.parse_args()

    errs, bpms, scores, allscores, wanders, jumps, biases = [], [], [], [], [], [], []
    locked = 0
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, 90.0, 320.0)
        if len(odf) < 1200:
            continue
        t = P.Tracker()          # the firmware's settings, in the twin
        t.run(odf)
        log = np.array(t.bpm_log)
        log = log[log > 0]
        if len(log) == 0:
            continue
        locked += 1
        lb = oracle_bpm(k)
        if lb > 0:
            errs.append(float(np.median(np.abs(log - lb))))
        bpms.append(float(np.median(log)))
        sc = np.array(t.score_log)
        sc = sc[sc > 0]
        if len(sc):
            scores.append(float(np.median(sc)))
            allscores.append(sc)
        half = log[len(log) // 2:]
        wanders.append(float(np.std(half)))
        jumps.append(int(np.sum(np.abs(np.diff(half)) > 0.5)))
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        if ons is not None and len(ons) and len(t.beats):
            b = np.array(t.beats) / P.RATE
            o = np.asarray(ons, dtype=float)
            d = b[:, None] - o[None, :]
            j = np.argmin(np.abs(d), axis=1)
            biases.append(float(np.median(d[np.arange(len(b)), j] * 1000.0)))

    e = np.array(errs); bm = np.array(bpms)
    print("the RATE, over %d windows (%d locked)" % (len(e), locked))
    print("  the error against librosa: median %.3f  mean %.3f  p90 %.3f BPM"
          % (np.median(e), e.mean(), np.percentile(e, 90)))
    print("  the rate: median %.2f  p10 %.2f  p90 %.2f BPM"
          % (np.median(bm), np.percentile(bm, 10), np.percentile(bm, 90)))
    print("  the wander %.3f BPM   the jumps %.2f a window"
          % (np.mean(wanders), np.mean(jumps)))
    print()
    sc = np.concatenate(allscores) if allscores else np.zeros(1)
    print("the SCORE, per frame")
    print("  p1 %.3f  p5 %.3f  p10 %.3f  median %.3f  p90 %.3f"
          % (np.percentile(sc, 1), np.percentile(sc, 5), np.percentile(sc, 10),
             np.median(sc), np.percentile(sc, 90)))
    print("  the floor (a phase-uniform signal): 0.210 at 90 BPM, 0.280 above 120")
    print("  the frames below the floor of 0.35: %.1f%%"
          % (100.0 * (sc < 0.35).mean()))
    print()
    if biases:
        b = np.array(biases)
        print("the PHASE against the kick: median %+.1f ms  mean %+.1f ms"
              % (np.median(b), b.mean()))

    # ---- the DJ-style mix ------------------------------------------------
    print()
    picks = []
    seen = set()
    for k in _cache.keys()[:400]:
        tr = k.split("@")[0]
        if tr in seen:
            continue
        seen.add(tr)
        picks.append(k)
        if len(picks) >= args.mix:
            break
    mix = []
    for k in picks:
        x = _cache.load(k)
        if x is None:
            continue
        mix.append(np.asarray(x, dtype=np.float64))
        mix.append(np.zeros(int(0.05 * _cache.CACHE_RATE)))
    odf = B.firmware_odf(np.concatenate(mix), 90.0, 320.0)
    t = P.Tracker()
    t.run(odf)
    log = np.array(t.bpm_log)
    log[log <= 0] = np.nan
    print("the DJ-style MIX of %d tracks (librosa: %s)"
          % (len(picks), " ".join("%.0f" % oracle_bpm(k) for k in picks)))
    n = len(log)
    step = max(1, n // 18)
    print("  the rate over time: %s"
          % " ".join("%.0f" % log[i] for i in range(0, n, step)))
    good = log[log > 0]
    print("  median %.1f  max %.1f  min %.1f   (the old kernel climbed to 175)"
          % (np.median(good), good.max(), good.min()))


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""The DJ-style MIX, as the firmware has it.

This is the live symptom: a set of house music at about 125 BPM, played one
track after another, and the board reporting 167-175.  The mix is built from
the cached tracks with a short gap between them, and the rate is read over
the whole mix.

The measurement that matters is not the median rate - it is the HIGH rate:
the music never goes above 131 in this corpus, so anything the tracker reads
above 140 is a false rate, and the length of time it spends above 140 is how
long the board would show a beat strobe that is not there.

    python tools/ar_sim/_fw_mix.py --tracks 12
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402
from _fw_check import oracle_bpm                   # noqa: E402


def build_mix(ntracks):
    """The tracks, in order, with a short gap between them."""
    picks, seen = [], set()
    for k in _cache.keys()[:400]:
        tr = k.split("@")[0]
        if tr in seen:
            continue
        seen.add(tr)
        picks.append(k)
        if len(picks) >= ntracks:
            break
    mix = []
    for k in picks:
        x = _cache.load(k)
        if x is None:
            continue
        mix.append(np.asarray(x, dtype=np.float64))
        mix.append(np.zeros(int(0.05 * _cache.CACHE_RATE)))
    return picks, np.concatenate(mix)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tracks", type=int, default=12)
    ap.add_argument("--collapse", type=int, default=None)
    ap.add_argument("--dead-margin", type=float, default=None)
    ap.add_argument("--min-score", type=float, default=None)
    args = ap.parse_args()

    picks, mix = build_mix(args.tracks)
    truth = [oracle_bpm(k) for k in picks]
    odf = B.firmware_odf(mix, 90.0, 320.0)
    t = P.Tracker()
    if args.collapse is not None:
        t.collapse_count_max = args.collapse
    if args.dead_margin is not None:
        t.dead_margin = args.dead_margin
    if args.min_score is not None:
        t.min_score = args.min_score
    t.run(odf)

    log = np.array(t.bpm_log)
    log[log <= 0] = np.nan
    good = log[log > 0]
    print("the DJ-style MIX of %d tracks (librosa: %s)"
          % (len(picks), " ".join("%.0f" % t_ for t_ in truth)))
    n = len(log)
    step = max(1, n // 18)
    print("  the rate over time: %s"
          % " ".join("%.0f" % log[i] for i in range(0, n, step)))
    print("  median %.1f  max %.1f  min %.1f"
          % (np.median(good), good.max(), good.min()))
    print("  the false rate: %.1f %% of the mix above 140 (the music never is)"
          % (100.0 * (good > 140).mean()))
    print("  the rate vs the truth: median %.2f  mean %.2f BPM"
          % (np.median(np.abs(good - np.median(truth))),
             np.mean(np.abs(good - np.median(truth)))))


if __name__ == "__main__":
    sys.exit(main())

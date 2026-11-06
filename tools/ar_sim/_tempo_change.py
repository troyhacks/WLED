#!/usr/bin/env python3
"""THE CONFUSION TIME: how long the tracker takes to follow a tempo change.

Troy's report: a drastic change - 170ish to 120ish - takes a long time to
follow, and the grid "wanders" down through the intermediate rates.

This measures it on a click track that changes tempo half way, and reports
the time from the change to the published rate settling on the new one.

    python tools/ar_sim/_tempo_change.py
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_port as P                               # noqa: E402
from _fw_band import firmware_odf                  # noqa: E402


def click_track(segments, seed=1):
    """segments: a list of (bpm, seconds).  A kick every beat."""
    rng = np.random.default_rng(seed)
    total = sum(s for _, s in segments)
    n = int(total * _cache.CACHE_RATE)
    x = rng.normal(0.0, 0.002, n)
    t = 0.0
    for bpm, dur in segments:
        period = 60.0 / bpm
        end = t + dur
        while t < end:
            i = int(t * _cache.CACHE_RATE)
            ln = int(0.18 * _cache.CACHE_RATE)
            if i + ln < n:
                env = np.exp(-np.arange(ln) / (0.035 * _cache.CACHE_RATE))
                body = 55.0 * np.sin(2.0 * np.pi * 62.0 * np.arange(ln) / _cache.CACHE_RATE)
                x[i:i + ln] += 0.9 * env * body
            t += period
    return x


def settle_time(segments, quant, bonus, tol=0.3, need=40, margin=None):
    """Seconds from the change until the published rate holds the new BPM."""
    x = click_track(segments)
    odf = firmware_odf(x, P.BAND_LO, P.BAND_HI)
    t = P.Tracker()
    t.quant = quant
    t.whole_bonus = bonus
    if margin is not None:
        t.net_margin = margin
    t.run(odf)
    log = np.array(t.bpm_log)
    if len(log) == 0:
        return None
    new_bpm = segments[-1][0]
    change_s = sum(s for _, s in segments[:-1])
    i0 = int(change_s * P.RATE)
    tail = log[i0:]
    if len(tail) == 0:
        return None
    on = np.abs(tail - new_bpm) <= tol
    # the first index from which the rate holds the new BPM for `need` frames
    run_len = 0
    for i, v in enumerate(on):
        run_len = run_len + 1 if v else 0
        if run_len >= need:
            return (i - run_len + 1) / P.RATE
    return float("nan")


CASES = [
    ((140.0, 30.0), (120.0, 30.0)),
    ((128.0, 30.0), (124.0, 30.0)),
    ((128.0, 30.0), (130.0, 30.0)),
    ((124.0, 30.0), (128.0, 30.0)),
    ((100.0, 30.0), (140.0, 30.0)),
]

MODES = [(0, 0.0), (1, 0.01)]
MARGINS = [None]


MARGIN = None


def main():
    print("%-22s %s" % ("change", "  ".join("%12s" % ("q%d b%.2f" % m) for m in MODES)))
    for segs in CASES:
        cells = []
        for q, b in MODES:
            st = settle_time(segs, q, b, margin=MARGIN)
            cells.append("%11.2f s" % st if st == st else "        never")
        label = "%.0f -> %.0f" % (segs[0][0], segs[-1][0])
        print("%-22s %s" % (label, "  ".join("%12s" % c for c in cells)))
    return 0


if __name__ == "__main__":
    global MARGIN
    if len(sys.argv) > 1:
        MARGIN = float(sys.argv[1])
    sys.exit(main())

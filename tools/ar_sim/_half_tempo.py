#!/usr/bin/env python3
"""THE HALF TEMPO: does the whole-number preference throw a real half away?

Troy saw a set that ran at 122.5 BPM for a while - and 122.5 is real, not a
detector error.  The lattice keeps the halves as candidates; the flat
whole-number BONUS then penalises them, so a 122.5 track could be dragged
to 122.0.

This makes a click track at a known rate and asks the tracker what it
publishes.  A kick every beat is the easiest case there is, so if the
bonus drags a half to a whole HERE, it will certainly do it on music.

    python tools/ar_sim/_half_tempo.py
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_port as P                               # noqa: E402
from _fw_band import firmware_odf                  # noqa: E402

DUR = 60.0


def click_track(bpm, dur=DUR, seed=1):
    """A kick every beat, at the cache's rate."""
    rng = np.random.default_rng(seed)
    n = int(dur * _cache.CACHE_RATE)
    x = rng.normal(0.0, 0.002, n)
    period = 60.0 / bpm
    t = 0.0
    while t < dur:
        i = int(t * _cache.CACHE_RATE)
        ln = int(0.18 * _cache.CACHE_RATE)
        if i + ln < n:
            env = np.exp(-np.arange(ln) / (0.035 * _cache.CACHE_RATE))
            body = 55.0 * np.sin(2.0 * np.pi * 62.0 * np.arange(ln) / _cache.CACHE_RATE)
            x[i:i + ln] += 0.9 * env * body
        t += period
    return x


def settled(t):
    log = np.array(t.bpm_log)
    if len(log) < 200:
        return log
    return log[-int(20.0 * P.RATE):]


def run(bpm, quant, bonus):
    odf = firmware_odf(click_track(bpm), P.BAND_LO, P.BAND_HI)
    t = P.Tracker()
    t.quant = quant
    t.whole_bonus = bonus
    t.run(odf)
    tr = settled(t)
    if len(tr) == 0:
        return None
    return {
        "true": bpm,
        "median": float(np.median(tr)),
        "wander": float(np.std(tr)),
        "err": float(np.median(tr) - bpm),
    }


def main():
    rates = [122.0, 122.5, 126.5, 128.0, 140.0]
    modes = [(0, 0.0), (1, 0.0), (1, 0.01), (1, 0.02), (1, 0.04)]
    print("%6s | %s" % ("true", "  ".join("%14s" % ("q%d b%.2f" % m) for m in modes)))
    for bpm in rates:
        cells = []
        for q, b in modes:
            r = run(bpm, q, b)
            cells.append("%7.2f e%+5.2f" % (r["median"], r["err"]))
        print("%6.1f | %s" % (bpm, "  ".join("%14s" % c for c in cells)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

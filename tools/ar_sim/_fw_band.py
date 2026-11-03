#!/usr/bin/env python3
"""THE BAND, PROPERLY.  The ODF's band, measured the way the board runs it.

WHAT WAS MEASURED BEFORE WAS NOT THE FIRMWARE
---------------------------------------------
The corpus numbers (0.817 and friends) were measured on `_device_odf.py`,
which builds the band filters for 3 kHz and RUNS them on the 6 kHz cache.
A digital filter built for a corner f at rate fs has that corner at
f * (the rate it runs at / fs) when it runs at a different rate, so the
40-160 Hz filters run at 6 kHz are really an 80-320 Hz band.  The corpus
was measured against a band the board will never have.

The board runs the OTHER order:

    the anti-alias filter at 300 Hz, on the 48 kHz stream
    one sample in sixteen is kept   ->  3 kHz
    the band filters, AT 3 kHz      ->  a true 40-160 Hz band

`_device_odf2.device_odf_fw` models that.  This script sweeps the band's
corners over that model and measures the hit rate for each.

THE METRIC'S BIAS, AND THE SECOND GROUND TRUTH
-----------------------------------------------
The hit rate is measured against `_diag_phase.kick_onsets`, which is itself
a 40-160 Hz kick detector.  A tracker whose band is exactly that band has
an unfair advantage: its onsets and the ground truth's onsets are the same
events.  So every band is measured against a SECOND ground truth as well -
a wide-band onset detector - and a band that wins against both is better
rather than merely self-consistent.

    python tools/ar_sim/_fw_band.py --limit 200
"""

import argparse
import math
import os
import sys
import time

import numpy as np
from scipy.signal import lfilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_port as P                               # noqa: E402
from _device_odf2 import butter_lp_sections       # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402

# The bands to try: (low corner, high corner), both in Hz at 3 kHz.
BANDS = [
    ("40-160", 40.0, 160.0),      # the firmware today
    ("80-320", 80.0, 320.0),      # what the corpus was measured on
    ("60-240", 60.0, 240.0),
    ("40-320", 40.0, 320.0),
    ("30-120", 30.0, 120.0),
    ("40-120", 40.0, 120.0),
    ("80-240", 80.0, 240.0),
    ("50-200", 50.0, 200.0),
]


def rbj_hpf(f0, fs, q):
    w0 = 2.0 * math.pi * f0 / fs
    cw = math.cos(w0)
    alpha = math.sin(w0) / (2.0 * q)
    a0 = 1.0 + alpha
    return [(1.0 + cw) * 0.5 / a0, -(1.0 + cw) / a0, (1.0 + cw) * 0.5 / a0,
            -2.0 * cw / a0, (1.0 - alpha) / a0]


def rbj_lpf(f0, fs, q):
    w0 = 2.0 * math.pi * f0 / fs
    cw = math.cos(w0)
    alpha = math.sin(w0) / (2.0 * q)
    a0 = 1.0 + alpha
    return [(1.0 - cw) * 0.5 / a0, (1.0 - cw) / a0, (1.0 - cw) * 0.5 / a0,
            -2.0 * cw / a0, (1.0 - alpha) / a0]


def band_sections(lo, hi, fs):
    """The firmware's band: two high-passes and two low-passes, at 3 kHz."""
    return [rbj_hpf(lo, fs, P.Q), rbj_hpf(lo, fs, P.Q),
            rbj_lpf(hi, fs, P.Q), rbj_lpf(hi, fs, P.Q)]


_AA_CACHE = {}


def aa_sections():
    """The anti-alias filter: the AR's own Butterworth, 4th order, 300 Hz."""
    if "c" not in _AA_CACHE:
        _AA_CACHE["c"] = butter_lp_sections(300.0, _cache.CACHE_RATE, 4, 2)
    return _AA_CACHE["c"]


def firmware_odf(x6k, lo, hi):
    """The firmware's ODF, faithfully: anti-alias, decimate, band at 3 kHz."""
    y = np.asarray(x6k, dtype=np.float64)
    for c in aa_sections():
        b0, b1, b2, a1, a2 = c
        y = lfilter([b0, b1, b2], [1.0, a1, a2], y)
    y = y[::2]                                   # 6 kHz -> 3 kHz
    for c in band_sections(lo, hi, 3000.0):
        b0, b1, b2, a1, a2 = c
        y = lfilter([b0, b1, b2], [1.0, a1, a2], y)
    n = (len(y) // P.HOP) * P.HOP
    if n == 0:
        return np.zeros(0)
    p = (y[:n].reshape(-1, P.HOP) ** 2).mean(axis=1)
    db = 10.0 * np.log10(p + 1e-12)
    return np.maximum(np.diff(db, prepend=db[0]), 0.0)


def wide_onsets(x6k):
    """A SECOND ground truth: onsets over a wide band, not just the kick.

    The same detector as `kick_onsets` - a zero-phase bandpass, a rise in
    the short-time energy, 35 % of the maximum, a 0.2 s gap - but over
    20-2000 Hz, so it catches the snare and the hats as well.  A band that
    wins against both ground truths is better, not merely self-consistent
    with the first.
    """
    import scipy.signal as ss
    sos = ss.butter(4, [20.0 / (_cache.CACHE_RATE / 2),
                        2000.0 / (_cache.CACHE_RATE / 2)],
                    btype="band", output="sos")
    y = ss.sosfiltfilt(sos, np.asarray(x6k, dtype=np.float64))
    hop = _cache.CACHE_HOP
    n = (len(y) // hop) * hop
    if n == 0:
        return np.zeros(0)
    p = (y[:n].reshape(-1, hop) ** 2).mean(axis=1)
    rise = np.maximum(np.diff(p, prepend=p[0]), 0.0)
    if rise.max() <= 0:
        return np.zeros(0)
    r = rise / rise.max()
    gap = max(1, int(0.20 * _cache.CACHE_RATE / hop))
    out = []
    last = -10 ** 9
    for i in np.where(r > 0.35)[0]:
        if i - last < gap:
            continue
        out.append(i * hop / float(_cache.CACHE_RATE))
        last = i
    return np.asarray(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=200)
    args = ap.parse_args()

    keys = _cache.keys()[:args.limit]
    hits = {b[0]: [] for b in BANDS}
    hits2 = {b[0]: [] for b in BANDS}
    peaks = {b[0]: [] for b in BANDS}
    bpms = {b[0]: [] for b in BANDS}
    t0 = time.time()
    n = 0
    for k in keys:
        x = _cache.load(k)
        if x is None:
            continue
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        ons2 = wide_onsets(x)
        if len(ons) < 4:
            continue
        for (name, lo, hi) in BANDS:
            odf = firmware_odf(x, lo, hi)
            if len(odf) < 1200:
                continue
            t = P.Tracker()
            t.run(odf)
            if not t.beats:
                hits[name].append(0.0)
                hits2[name].append(0.0)
                peaks[name].append(0)
                continue
            f, _off = P.hit_with_offset(np.array(t.beats) / P.RATE, ons)
            hits[name].append(f)
            f2, _off = P.hit_with_offset(np.array(t.beats) / P.RATE, ons2)
            hits2[name].append(f2)
            peaks[name].append(len(t.sweep_frames) if t.sweep_frames is not None
                               else 0)
            bpms[name].append(t.bpm)
        n += 1
        if n % 20 == 0:
            print("  %d/%d  (%.0f s)" % (n, len(keys), time.time() - t0),
                  flush=True)

    print()
    print("%d windows   %.0f s" % (n, time.time() - t0))
    print()
    print("%-8s | %-24s | %-24s | %6s %6s"
          % ("band", "vs the KICK onsets", "vs the WIDE onsets", "med", "peaks"))
    print("%-8s | %7s %6s %6s | %7s %6s %6s | %6s %6s"
          % ("", "median", ">=0.5", ">=0.8", "median", ">=0.5", ">=0.8",
             "bpm", "in win"))
    for (name, _lo, _hi) in BANDS:
        a = np.array(hits[name])
        b = np.array(hits2[name])
        pk = np.array(peaks[name])
        bm = np.array(bpms[name])
        print("%-8s | %7.3f %5.1f%% %5.1f%% | %7.3f %5.1f%% %5.1f%% | %6.1f %6.1f"
              % (name, float(np.median(a)), 100.0 * float((a >= 0.5).mean()),
                 100.0 * float((a >= 0.8).mean()),
                 float(np.median(b)), 100.0 * float((b >= 0.5).mean()),
                 100.0 * float((b >= 0.8).mean()),
                 float(np.median(bm)), float(pk.mean())))


if __name__ == "__main__":
    sys.exit(main())

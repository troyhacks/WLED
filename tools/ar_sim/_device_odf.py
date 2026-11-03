#!/usr/bin/env python3
"""Does the ODF survive at 3 kHz, the rate the firmware would run it at?

WHY THIS QUESTION EXISTS
------------------------
The firmware's AudioReactive already decimates 48 kHz to 3 kHz for the
bottom filter bank (AR_BANK_DEC = 16, anti-alias at 400 Hz).  The tempo ODF
wants the same 40-160 Hz band, so it could ride that stream and run its
frames at 3 kHz - 32 samples per hop instead of the offline simulation's 64.

But the offline simulation ran at 6 kHz (CACHE_RATE = 6000), so "3 kHz works"
is an assumption until it is measured.  A lower rate means:

  * fewer samples per frame, so the frame's power estimate is noisier, and
    the ODF is a DIFFERENCE of dB - noise in the estimate becomes noise in
    the difference;
  * the filters' designs differ (fs 3000 instead of 6000), so their
    transition bands differ;
  * the anti-alias ahead of the decimation is the bank's 400 Hz two-pole
    rather than the cache's polyphase resampler.

This emulates the DEVICE's structure from the 6 kHz cache:

    hp40 x2 -> lp160 x2, designed at fs = 3000
    decimate by 2                       (6 kHz -> 3 kHz)
    frames of 32 samples                (32/3000 s = 10.67 ms = the hop)

The emulation is if anything slightly PESSIMISTIC: the cache's 6 kHz audio
was anti-aliased at 3 kHz by a proper polyphase resampler, whereas the
device's bank anti-aliases at 400 Hz with two poles, so the device's
3 kHz stream has LESS junk folded down into the 40-160 Hz band than this
emulation does.

Then the whole estimator runs on it - peak picking, the comb, the metric -
and the numbers are compared against the 6 kHz baseline.

    python tools/ar_sim/_device_odf.py --limit 100
"""

import argparse
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _tempo_acf as T                             # noqa: E402
import _beat as B                                  # noqa: E402
import _comb as C                                  # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402
from _eval_tempo import hit_with_offset, grid_at   # noqa: E402

DEVICE_FS = 3000.0          # the bank's decimated rate
DEVICE_HOP = 32             # 32 / 3000 s = 10.667 ms = the firmware's hop
DEVICE_RATE = DEVICE_FS / DEVICE_HOP              # 93.75 fps, exactly


def device_odf(x6k, fs=DEVICE_FS):
    """The ODF the firmware would produce, emulated from the 6 kHz cache."""
    from scipy.signal import lfilter
    d = T.designs(fs=fs)
    secs = d["hp40_lp160x2"]
    y = x6k.astype(np.float64)
    for c in secs:
        b0, b1, b2, a1, a2 = c
        y = lfilter([b0, b1, b2], [1.0, a1, a2], y)
    # decimate to fs
    dec = int(round(_cache.CACHE_RATE / fs))
    y = y[::dec]
    n = (len(y) // DEVICE_HOP) * DEVICE_HOP
    if n == 0:
        return np.zeros(0)
    frames = y[:n].reshape(-1, DEVICE_HOP)
    p = (frames ** 2).mean(axis=1)
    db = 10.0 * np.log10(p + 1e-12)
    return np.maximum(np.diff(db, prepend=db[0]), 0.0)


def run(odf, x, t_lo, t_hi, prior, weight, rate):
    """The estimator on one ODF.  Returns (bpm, score, hit)."""
    mx = float(np.max(odf))
    if mx <= 0:
        return None, 0.0, 0.0
    odfn = odf / mx
    peaks, strengths = B.peak_pick(odfn)
    win = int(12.0 * rate)
    first = max(0, len(odfn) - win)
    keep = peaks >= first
    peaks = peaks[keep]
    strengths = strengths[keep]
    if len(peaks) < 8:
        return None, 0.0, 0.0
    # the comb's period is in frames; the frame rate is 93.75 either way
    bpm, score, phase, _d = C.comb_rate(peaks, strengths, step=0.25,
                                        prior_bpm=prior, prior_weight=weight)
    g = grid_at(bpm, phase, first, len(odfn))
    ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
    wo = ons[(ons >= t_lo - 0.02) & (ons <= t_hi)]
    if len(wo) < 4:
        return bpm, score, 0.0
    f, _off = hit_with_offset(bpm, g / rate, wo)
    return bpm, score, f


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=100)
    ap.add_argument("--prior", type=float, default=125.0)
    ap.add_argument("--weight", type=float, default=0.6)
    ap.add_argument("--show", type=int, default=12)
    args = ap.parse_args()

    keys = _cache.keys()[:args.limit]
    res = {"6k": [], "3k": []}
    rows = []
    t0 = time.time()
    for k in keys:
        x = _cache.load(k)
        if x is None:
            continue
        o6 = T.odf_cached(k, x, "hp40_lp160x2", 0.0)
        first = max(0, len(o6) - int(12.0 * T.RATE))
        t_lo = first / T.RATE
        t_hi = len(o6) / T.RATE
        if t_hi - t_lo < 4.0:
            continue
        b6, s6, f6 = run(o6, x, t_lo, t_hi, args.prior, args.weight, T.RATE)
        o3 = device_odf(x)
        if len(o3) < 100:
            continue
        b3, s3, f3 = run(o3, x, t_lo, t_hi, args.prior, args.weight, DEVICE_RATE)
        if f6 == 0.0 and f3 == 0.0:
            continue
        res["6k"].append(f6)
        res["3k"].append(f3)
        rows.append((k, b6, f6, b3, f3, s3))

    print("%d windows   %.0f s" % (len(rows), time.time() - t0))
    print()
    print("the beat grid's hit rate on the raw onsets, constant offset removed")
    print()
    for name in ("6k", "3k"):
        a = np.array(res[name])
        print("  %-4s median %5.3f   >=0.5: %5.1f%%   >=0.8: %5.1f%%"
              % (name, float(np.median(a)), 100.0 * float((a >= 0.5).mean()),
                 100.0 * float((a >= 0.8).mean())))
    print()
    print("where 3 kHz is worse than 6 kHz:")
    worse = [r for r in rows if r[4] < r[2] - 0.10]
    for k, b6, f6, b3, f3, s3 in sorted(worse, key=lambda r: r[2] - r[4])[:args.show]:
        print("  %-42s 6k %7.2f (hit %4.2f)   3k %7.2f (hit %4.2f, score %4.2f)"
              % (k[:42], b6 if b6 else 0.0, f6, b3 if b3 else 0.0, f3, s3))
    print("  %d of %d windows" % (len(worse), len(rows)))


if __name__ == "__main__":
    sys.exit(main())

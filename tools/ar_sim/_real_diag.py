#!/usr/bin/env python3
"""What the estimator SEES on a real track.

The synthetic tracks scored 0 wrong of 48.  The real ones do not, and the
question is why.  This dumps the evidence: the ODF's in-band spectrum, its
local maxima, what the tie-break chose and what it rejected.

    python tools/ar_sim/_real_diag.py --file "004. Andrea" --window 150
    python tools/ar_sim/_real_diag.py --file "004. Andrea" --spectrum
"""

import argparse
import glob
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _tempo3_sim import (                      # noqa: E402
    SR, RATE, LEN, NFFT, K_MIN, K_MAX, filter_designs, apply_sections,
    bin_bpm, local_peaks, peak_in,
)
from _real_sim import MUSIC_DIR, decode, odf_of   # noqa: E402


def show(path, design, start_s, span=40.0):
    x = decode(path, start_s, span)
    print("%s   window at %.0f s   %.1f s of audio" %
          (os.path.basename(path), start_s, len(x) / SR))
    odf = odf_of(x, design)
    print("ODF: %d frames   mean %.4f   max %.4f" %
          (len(odf), odf.mean(), odf.max()))

    win = odf[-LEN:].astype(np.float64)
    P = np.abs(np.fft.rfft(win, NFFT))
    mags = P[K_MIN:K_MAX + 1]

    # the whole spectrum, in the band, as a bar chart
    print()
    print("the in-band spectrum (each row is one candidate BPM):")
    top = float(mags.max())
    for bpm, m in local_peaks(mags, K_MIN, K_MAX):
        rel = 20.0 * math.log10(max(m, 1e-30) / top)
        bar = "#" * max(0, int((rel + 40.0) * 1.6))
        print("  %7.2f BPM  %7.1f dB  %s" % (bpm, rel, bar))

    best, mag = peak_in(mags, K_MIN, K_MAX)
    print()
    print("tallest in-band peak: %.2f BPM" % best)

    # what the tie-break would do
    thr = mag / (10.0 ** (3.0 / 20.0))
    print("tie-break threshold (3 dB): %.4f" % thr)
    for b, m in local_peaks(mags, K_MIN, K_MAX):
        if b >= best:
            continue
        if m <= 0 or m < thr:
            print("  %.2f BPM: rejected, %.1f dB below the peak" %
                  (b, 20 * math.log10(max(m, 1e-30) / max(mag, 1e-30))))
            continue
        r = best / b
        hits = []
        for q in range(1, 5):
            for p in range(q + 1, 5):
                if abs(r - p / q) < 0.01 * (p / q):
                    hits.append("%d/%d" % (p, q))
        print("  %.2f BPM: ratio %.4f -> %s" %
              (b, r, ("ACCEPTED as " + hits[0]) if hits else "not a simple ratio"))

    # the same window with a longer one, to see whether it is the window
    print()
    for secs in (8.0, 12.0, 16.0, 24.0, 32.0):
        n = int(secs * RATE)
        if len(odf) < n:
            continue
        w = odf[-n:].astype(np.float64)
        Q = np.abs(np.fft.rfft(w, 1 << int(math.ceil(math.log2(4 * n)))))
        bestq = None
        bq = 0.0
        for k in range(K_MIN, K_MAX + 1):
            if Q[k] > bq:
                bq = Q[k]
                bestq = k
        # the spectrum's own grid, not the 4096 one
        bpm_q = bestq * RATE / (1 << int(math.ceil(math.log2(4 * n)))) * 60.0
        print("  window %4.1f s -> %7.2f BPM (bin %d of %d)" %
              (secs, bpm_q, bestq, 1 << int(math.ceil(math.log2(4 * n)))))

    # ---- the ODF itself -------------------------------------------------
    print()
    print("the ODF, 150 frames (one column = one frame = %.1f ms):" % (1000.0 / RATE))
    seg = odf[-LEN:][:150]
    top = float(seg.max())
    for i in range(0, len(seg), 50):
        row = seg[i:i + 50]
        print("  %s" % "".join(
            " .:-=+*#%@"[min(9, int(v / top * 9.999))] for v in row))

    # ---- the autocorrelation -------------------------------------------
    #
    # If the ODF is periodic, its autocorrelation has a peak at the beat
    # period.  If the autocorrelation is flat, the ODF is the problem and
    # no spectrum of it will help.
    print()
    print("the ODF's autocorrelation, by lag:")
    w = odf[-LEN:].astype(np.float64)
    w = w - w.mean()
    ac = np.correlate(w, w, mode="full")[len(w) - 1:]
    ac /= ac[0]
    lag_beats = {}
    for lag in range(2, len(ac)):
        bpm = 60.0 * RATE / lag
        if 85.0 <= bpm <= 180.0:
            lag_beats.setdefault(round(bpm, 1), []).append((lag, ac[lag]))
    top_lags = sorted(lag_beats.items(),
                      key=lambda kv: -max(v for _, v in kv[1]))[:8]
    for bpm, lst in top_lags:
        lag, v = max(lst, key=lambda t: t[1])
        print("  lag %4d frames (%.4f s)  %7.2f BPM   r = %.3f" %
              (lag, lag / RATE, bpm, v))
    return mags


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", default="004. Andrea")
    ap.add_argument("--window", type=float, default=150.0)
    ap.add_argument("--design", default="hp40_lp160x2")
    args = ap.parse_args()

    fs = glob.glob(os.path.join(MUSIC_DIR, "*.mp3"))
    sel = [f for f in fs if args.file in os.path.basename(f)]
    if not sel:
        print("no match")
        return 1
    show(sel[0], args.design, args.window)
    return 0


if __name__ == "__main__":
    sys.exit(main())

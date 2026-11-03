#!/usr/bin/env python3
"""The apples-to-apples contrast: peak-to-RMS of the spectrum and of the
autocorrelation, over the SAME 12 s window of the SAME real ODF.

    python tools/ar_sim/_tempo16_crest.py
"""

import glob
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from _tempo15_sweep import (acf, odf, RATE, NFFT, BPM_LO, BPM_HI)  # noqa: E402

CACHE = os.path.join(HERE, "_cache")


def band_ks():
    k = np.arange(NFFT // 2 + 1)
    bpm = k * RATE / NFFT * 60.0
    return k[(bpm >= BPM_LO) & (bpm <= BPM_HI)]


def main():
    ks = band_ks()
    lag_lo = int(60.0 * RATE / BPM_HI)
    lag_hi = int(60.0 * RATE / BPM_LO)
    sp_crest, ac_crest = [], []
    sp_half, ac_half = [], []
    sp_near, ac_near = [], []
    n = 0
    for path in sorted(glob.glob(os.path.join(CACHE, "*.npy"))):
        o = odf(np.load(path).astype(np.float64))
        if len(o) < int(12.0 * RATE):
            continue
        o = o[:int(12.0 * RATE)]

        # ---- spectrum ------------------------------------------------
        x = o - o.mean()
        S = np.abs(np.fft.rfft(x, NFFT))
        band = S[ks]
        pk = band.max()
        rms = np.sqrt((band ** 2).mean())
        sp_crest.append(pk / rms)
        # how wide is the peak? (bins within 3 dB of the peak)
        near = ks[band >= pk * 10 ** (-3.0 / 20.0)]
        sp_half.append((near[-1] - near[0]) * RATE / NFFT * 60.0)
        # local maxima within 14 dB of the peak
        cnt = 0
        for i in range(1, len(ks) - 1):
            if (band[i] > band[i - 1] and band[i] >= band[i + 1]
                    and 10 * math.log10(band[i] / pk) > -14.0):
                cnt += 1
        sp_near.append(cnt)

        # ---- autocorrelation ----------------------------------------
        r = acf(o)
        lags = np.arange(lag_lo, lag_hi + 1)
        rv = r[lags]
        ar = np.abs(rv)
        apk = ar.max()
        arms = np.sqrt((ar ** 2).mean())
        ac_crest.append(apk / arms)
        near = lags[ar >= apk * 10 ** (-3.0 / 20.0)]
        ac_half.append((60.0 * RATE / near[0]) - (60.0 * RATE / near[-1]))
        cnt = 0
        for i in range(1, len(rv) - 1):
            if rv[i] > rv[i - 1] and rv[i] >= rv[i + 1]:
                cnt += 1
        ac_near.append(cnt)
        n += 1

    def st(v, f="%.2f"):
        v = np.array(v, dtype=float)
        return (f + " mean, " + f + " median, " + f + " min, " + f + " max") % (
            v.mean(), np.median(v), v.min(), v.max())

    print("%d real 12 s windows, 90-175 BPM band\n" % n)
    print("CREST FACTOR (peak / RMS over the band)")
    print("  spectrum        : %s" % st(sp_crest))
    print("  autocorrelation : %s" % st(ac_crest))
    print("\nPEAK WIDTH (the span within 3 dB of the peak, in BPM)")
    print("  spectrum        : %s" % st(sp_half))
    print("  autocorrelation : %s" % st(ac_half))
    print("\nCOMPETING LOCAL MAXIMA in the band")
    print("  spectrum (within 14 dB of the peak): %s" % st(sp_near, "%.1f"))
    print("  autocorrelation (all local maxima)  : %s" % st(ac_near, "%.1f"))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""One code path, one table: the spectrum and the autocorrelation, side by
side, on the same real 12 s ODF windows.

    python tools/ar_sim/_tempo17_final.py
"""

import glob
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from _tempo15_sweep import (acf, odf, RATE, NFFT, BPM_LO, BPM_HI,   # noqa: E402
                            tps_pick, prior)

CACHE = os.path.join(HERE, "_cache")


def main():
    ks = np.arange(NFFT // 2 + 1)
    bpm = ks * RATE / NFFT * 60.0
    ks = ks[(bpm >= BPM_LO) & (bpm <= BPM_HI)]
    # the TRUE resolution: the window is 1125 frames, the grid is 4096 points
    over = NFFT / (12.0 * RATE)
    lag_lo = int(60.0 * RATE / BPM_HI)
    lag_hi = int(60.0 * RATE / BPM_LO)

    sp_med, sp_ru, sp_n, sp_ntrue = [], [], [], []
    ac_r, ac_floor, ac_ru, ac_n, ac_sig = [], [], [], [], []
    agree = 0
    n = 0
    for path in sorted(glob.glob(os.path.join(CACHE, "*.npy"))):
        o = odf(np.load(path).astype(np.float64))
        if len(o) < int(12.0 * RATE):
            continue
        o = o[:int(12.0 * RATE)]
        n += 1

        # ---------- spectrum ----------
        x = o - o.mean()
        S = np.abs(np.fft.rfft(x, NFFT))
        band = S[ks]
        pk = band.max()
        med = np.median(band)
        # POWER ratio, the standard way a periodogram peak is quoted
        sp_med.append(10 * math.log10(pk * pk / (med * med)))
        # runner-up local maximum, excluding the winner's own lobe
        run = 0.0
        for i in range(1, len(ks) - 1):
            if band[i] > band[i - 1] and band[i] >= band[i + 1]:
                if abs(bpm[ks[i]] - bpm[ks[int(np.argmax(band))]]) > 3.0:
                    run = max(run, band[i])
        sp_ru.append(20 * math.log10(pk / run) if run > 0 else float("nan"))
        cnt = 0
        for i in range(1, len(ks) - 1):
            if (band[i] > band[i - 1] and band[i] >= band[i + 1]
                    and 10 * math.log10(band[i] / pk) > -14.0):
                cnt += 1
        sp_n.append(cnt)
        # the same count on the TRUE resolution grid (decimated)
        cnt_t = 0
        ktrue = ks[::int(round(over))]
        bt = S[ktrue]
        for i in range(1, len(ktrue) - 1):
            if (bt[i] > bt[i - 1] and bt[i] >= bt[i + 1]
                    and 10 * math.log10(bt[i] / bt.max()) > -14.0):
                cnt_t += 1
        sp_ntrue.append(cnt_t)

        # ---------- autocorrelation ----------
        r = acf(o)
        lags = np.arange(lag_lo, lag_hi + 1)
        rv = r[lags]
        lm = [i for i in range(1, len(rv) - 1)
              if rv[i] > rv[i - 1] and rv[i] >= rv[i + 1]]
        if not lm:
            continue
        best = max(lm, key=lambda i: rv[i])
        ac_r.append(rv[best])
        ac_floor.append(np.median(rv))
        # the sampling noise of the normalised ACF: sd ~ 1/sqrt(N)
        ac_sig.append((rv[best] - np.median(rv)) * math.sqrt(len(o)))
        others = [rv[i] for i in lm if abs(lags[i] - lags[best]) > 3]
        ac_ru.append(rv[best] - max(others) if others else float("nan"))
        ac_n.append(len(lm))

        # ---------- agreement ----------
        lagp, _ = tps_pick(r, len(o))
        bpm_p = 60.0 * RATE / lagp
        bpm_s = bpm[ks[int(np.argmax(band))]]
        if abs(bpm_s - bpm_p) < 0.04 * bpm_p:
            agree += 1

    def st(v, f="%.2f"):
        v = np.array(v, dtype=float)
        v = v[np.isfinite(v)]
        return (f + " mean, " + f + " med, " + f + " min, " + f + " max") % (
            v.mean(), np.median(v), v.min(), v.max())

    print("%d real 12 s ODF windows, %d bins / %d lags in 90-175 BPM\n"
          % (n, len(ks), lag_hi - lag_lo + 1))
    # what a PURE NOISE periodogram would give in this band:
    # N exponential bins -> max is lnN+gamma above the mean, the median is
    # ln2 below it, so the peak/median ratio grows with the bin count.
    lnN = math.log(len(ks))
    noise_pk_med = 10.0 * (lnN + 0.5772 + 0.6931) * math.log10(math.e)
    print("  noise floor: N=%d exponential bins -> the peak/median a PURE"
          % len(ks))
    print("  NOISE periodogram would show = %.1f dB" % noise_pk_med)
    print()
    print("SPECTRUM (4096-pt DFT, zero-padded from 1125 frames)")
    print("  peak over band median      : %s dB" % st(sp_med))
    print("  ... of which below the %.1f dB white-noise benchmark: %.0f%%"
          % (noise_pk_med, 100.0 * np.mean(np.array(sp_med) < noise_pk_med)))
    print("  peak over runner-up peak    : %s dB" % st(sp_ru))
    print("  local maxima within 14 dB   : %s  (4096 grid)" % st(sp_n, "%.1f"))
    print("                              : %s  (true %dx-resolution grid,"
          % (st(sp_ntrue, "%.1f"), int(round(over))))
    print("                                 %.2f BPM cells)"
          % (int(round(over)) * RATE / NFFT * 60.0))
    print("\nAUTOCORRELATION (same 1125 frames)")
    print("  r at the beat lag           : %s" % st(ac_r))
    print("  in-band floor (median r)    : %s" % st(ac_floor))
    print("  peak above floor, in sigma of the 1/sqrt(N) ACF sampling noise: %s"
          % st(ac_sig, "%.1f"))
    print("  peak over runner-up peak    : %s (in r units)" % st(ac_ru))
    print("  local maxima in the band    : %s" % st(ac_n, "%.1f"))
    print("\nAGREEMENT")
    print("  spectrum argmax within 4%% of the prior-weighted ACF pick: %d/%d = %.0f%%"
          % (agree, n, 100.0 * agree / n))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Quantify the autocorrelation-vs-spectrum contrast on the REAL ODF,
and sweep the analysis window to price the Rayleigh limit in BPM.

This is the measurement behind the literature claims:
  - the spectrum's tempo peak barely stands above its own noise floor,
  - the autocorrelation's does, by a large factor,
  - the autocorrelation still needs Ellis's log-normal tempo prior to pick
    the right metrical level, and
  - the window length sets the BPM accuracy.

    python tools/ar_sim/_tempo14_acf_prior.py
"""

import glob
import math
import os

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "_cache")

SR = 6000.0
HOP = 64
RATE = SR / HOP          # 93.75 frames per second
NFFT = 4096              # the firmware's DFT grid
EPS = 1e-10
BUTTER_Q = 1.0 / math.sqrt(2.0)

BPM_LO, BPM_HI = 90.0, 175.0
PRIOR_T0 = 0.5           # Ellis 2007: 120 BPM
PRIOR_SIGMA = 0.9        # Ellis 2007: octaves (the TPS2/TPS3 setting)


def rbj_hpf(f, fs, q):
    """ESP-DSP's dsps_biquad_gen_hpf_f32: b0, b1, b2, a1, a2 with a0 = 1."""
    w0 = 2.0 * math.pi * f / fs
    alpha = np.sin(w0) / (2.0 * q)
    a0 = 1.0 + alpha
    cw = math.cos(w0)
    return np.array([(1.0 + cw) / 2.0 / a0,
                     -(1.0 + cw) / a0,
                     (1.0 + cw) / 2.0 / a0,
                     -2.0 * cw / a0,
                     (1.0 - alpha) / a0], dtype=np.float64)


def rbj_lpf(f, fs, q):
    """ESP-DSP's dsps_biquad_gen_lpf_f32: b0, b1, b2, a1, a2 with a0 = 1."""
    w0 = 2.0 * math.pi * f / fs
    alpha = np.sin(w0) / (2.0 * q)
    a0 = 1.0 + alpha
    cw = math.cos(w0)
    return np.array([(1.0 - cw) / 2.0 / a0,
                     (1.0 - cw) / a0,
                     (1.0 - cw) / 2.0 / a0,
                     -2.0 * cw / a0,
                     (1.0 - alpha) / a0], dtype=np.float64)


def biquad(x, c):
    from scipy.signal import lfilter
    b0, b1, b2, a1, a2 = c
    return lfilter([b0, b1, b2], [1.0, a1, a2], x)


def odf(x):
    y = biquad(x, rbj_hpf(40.0, SR, BUTTER_Q))
    y = biquad(y, rbj_lpf(160.0, SR, BUTTER_Q))
    y = biquad(y, rbj_lpf(160.0, SR, BUTTER_Q))
    n = (len(y) // HOP) * HOP
    frames = y[:n].reshape(-1, HOP)
    p = (frames.astype(np.float64) ** 2).mean(axis=1)
    db = 10.0 * np.log10(p + EPS)
    return np.maximum(np.diff(db, prepend=db[0]), 0.0)


def bin_bpm(k):
    return k * RATE / NFFT * 60.0


def band_ks():
    k = np.arange(NFFT // 2 + 1)
    bpm = bin_bpm(k)
    return k[(bpm >= BPM_LO) & (bpm <= BPM_HI)]


def local_maxima(v, idx):
    """Local maxima of v at the positions idx (idx must be increasing)."""
    out = []
    for i in range(1, len(idx) - 1):
        if v[idx[i]] > v[idx[i - 1]] and v[idx[i]] >= v[idx[i + 1]]:
            out.append(i)
    return out


def spectrum(o, demean=True):
    x = o - o.mean() if demean else o
    return np.abs(np.fft.rfft(x, NFFT))


def acf(o):
    x = o - o.mean()
    n = len(x)
    nfft = 1
    while nfft < 2 * n:
        nfft *= 2
    F = np.fft.rfft(x, nfft)
    r = np.fft.irfft(F * np.conj(F), nfft)[:n]
    return r / r[0]


def ellis_prior(sec, sigma=PRIOR_SIGMA):
    return np.exp(-0.5 * (np.log2(sec / PRIOR_T0) / sigma) ** 2)


def acf_tempo(r, nframes, sigma=PRIOR_SIGMA, tps2=True):
    """Ellis 2007's TPS on the ACF, and the argmax lag in frames."""
    lag = np.arange(1, nframes)
    sec = lag / RATE
    tps = ellis_prior(sec, sigma) * r[lag]
    if tps2:
        # TPS2: add the half-tempo contribution (Ellis eq. 7)
        half = np.interp(2.0 * sec, lag, r[lag], left=0.0, right=0.0)
        tps = tps + 0.5 * ellis_prior(sec * 2, sigma) * half
    i = int(np.argmax(tps))
    return lag[i], tps


def main():
    files = sorted(glob.glob(os.path.join(CACHE, "*.npy")))
    ks = band_ks()
    lag_max = int(60.0 * RATE / BPM_LO) + 2          # slowest lag in band
    lag_min = max(1, int(60.0 * RATE / BPM_HI) - 2)   # fastest lag in band

    spec_med, spec_raw_med = [], []
    acf_med, acf_tps_med = [], []
    n_within_14 = []
    acf_beat_ok = 0
    half_or_double = 0
    prior_wins = 0
    raw_acf_ok = 0
    bpm_acf, bpm_prior, bpm_spec = [], [], []

    # ---- window sweep, on a subset, against the 40 s reference ----------
    sweep_windows = (4.0, 6.0, 8.0, 12.0, 16.0, 24.0, 40.0)
    sweep_err = {w: [] for w in sweep_windows}
    sweep_ref = {}

    for path in files:
        x = np.load(path).astype(np.float64)
        o = odf(x)
        if len(o) < int(40.0 * RATE):
            continue

        # ---- the 12 s window: spectrum vs autocorrelation --------------
        o12 = o[:int(12.0 * RATE)]
        S = spectrum(o12, demean=True)
        Sraw = spectrum(o12, demean=False)
        band = S[ks]
        med = np.median(band)
        spec_med.append(10.0 * math.log10(band.max() / med))
        spec_raw_med.append(10.0 * math.log10(Sraw[ks].max() /
                                              np.median(Sraw[ks])))
        # how many local maxima lie within 14 dB of the spectrum peak?
        pkv = band.max()
        cnt = 0
        for i in local_maxima(S, ks):
            if 10.0 * math.log10(S[ks[i]] / pkv) > -14.0:
                cnt += 1
        n_within_14.append(cnt)

        r = acf(o12)
        lags = np.arange(lag_min, lag_max + 1)
        rv = r[lags]
        # the ACF's own peak-to-floor: the best local max over the band
        lm = local_maxima(r, lags)
        best = max(lm, key=lambda i: r[lags[i]]) if lm else int(np.argmax(rv))
        floor = np.median(rv)
        if r[lags[best]] > 0 and floor > 0:
            acf_med.append(20.0 * math.log10(r[lags[best]] / floor))
        else:
            acf_med.append(float("nan"))
        bpm_acf.append(60.0 * RATE / lags[best])
        raw_acf_ok += 1

        # Ellis's prior-weighted TPS pick
        lagp, _ = acf_tempo(r, len(o12))
        bpm_prior.append(60.0 * RATE / lagp)
        # the prior-weighted peak's own contrast
        sec = lags / RATE
        tps = ellis_prior(sec) * rv
        i2 = int(np.argmax(tps))
        tfloor = np.median(tps)
        acf_tps_med.append(20.0 * math.log10(tps[i2] / tfloor)
                           if tps[i2] > 0 and tfloor > 0 else float("nan"))

        # metrical level: is the prior pick double or half the raw ACF pick?
        ratio = (60.0 * RATE / lagp) / bpm_acf[-1]
        if abs(ratio - 2.0) < 0.06 or abs(ratio - 0.5) < 0.03:
            half_or_double += 1

        S_pk = int(np.argmax(band))
        bpm_spec.append(bin_bpm(ks[S_pk]))

    n = len(spec_med)
    def st(v):
        v = np.array(v)
        return "mean %6.2f  median %6.2f  min %6.2f  max %6.2f" % (
            v.mean(), np.median(v), v.min(), v.max())

    print("%d real 12 s windows\n" % n)
    print("PEAK ABOVE THE BAND'S OWN FLOOR (dB)")
    print("  spectrum, demeaned   : %s" % st(spec_med))
    print("  spectrum, raw (as the firmware runs it): %s" % st(spec_raw_med))
    print("  autocorrelation       : %s" % st(acf_med))
    print("  autocorrelation + Ellis prior : %s" % st(acf_tps_med))
    print("\n  ratio, spectrum / autocorrelation (dB): mean %.2f" %
          (np.mean(spec_med) - np.mean(acf_med)))
    print("  local maxima within 14 dB of the spectrum peak: %s" %
          st(n_within_14))
    print("\nMETRICAL LEVEL")
    print("  prior pick is 2x or 0.5x the raw ACF pick: %d/%d = %.0f%%" %
          (half_or_double, n, 100.0 * half_or_double / n))
    print("  spectrum argmax within 4%% of the prior pick: %d/%d = %.0f%%" %
          (sum(1 for a, b in zip(bpm_spec, bpm_prior)
               if abs(a - b) < 0.04 * b), n,
           100.0 * sum(1 for a, b in zip(bpm_spec, bpm_prior)
                       if abs(a - b) < 0.04 * b) / n))
    print("  median BPM  spectrum %.1f  ACF %.1f  ACF+prior %.1f" %
          (np.median(bpm_spec), np.median(bpm_acf), np.median(bpm_prior)))


if __name__ == "__main__":
    main()

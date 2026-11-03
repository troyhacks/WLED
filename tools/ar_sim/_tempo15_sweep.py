#!/usr/bin/env python3
"""The window sweep and the prior ablation, on the REAL ODF.

Three questions, three measurements:

  1. HOW ACCURATE is the prior-weighted autocorrelation on real house music?
     (the spread of its BPM answer over 298 windows)
  2. IS THE PRIOR DOING THE WORK?  Re-run with the prior centre moved to
     90, 120 and 150 BPM.  If the answer barely moves, the estimator is
     tracking the music; if it moves with the prior, it is not.
  3. WHAT DOES THE WINDOW COST?  Sweep the window length and price the
     Rayleigh limit in BPM.

    python tools/ar_sim/_tempo15_sweep.py
"""

import glob
import math
import os

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "_cache")

SR, HOP, NFFT = 6000.0, 64, 4096
RATE = SR / HOP
EPS = 1e-10
BUTTER_Q = 1.0 / math.sqrt(2.0)
BPM_LO, BPM_HI = 90.0, 175.0


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
                     (1.0 - alpha) / a0])


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
                     (1.0 - alpha) / a0])


def biquad(x, c):
    from scipy.signal import lfilter
    b0, b1, b2, a1, a2 = c
    return lfilter([b0, b1, b2], [1.0, a1, a2], x)


def odf(x):
    y = biquad(x, rbj_hpf(40.0, SR, BUTTER_Q))
    y = biquad(y, rbj_lpf(160.0, SR, BUTTER_Q))
    y = biquad(y, rbj_lpf(160.0, SR, BUTTER_Q))
    y = y[int(1.0 * SR):]          # let the band-pass settle
    n = (len(y) // HOP) * HOP
    fr = y[:n].reshape(-1, HOP)
    p = (fr.astype(np.float64) ** 2).mean(axis=1)
    db = 10.0 * np.log10(p + EPS)
    return np.maximum(np.diff(db, prepend=db[0]), 0.0)


def acf(o):
    x = o - o.mean()
    n = len(x)
    nf = 1
    while nf < 2 * n:
        nf *= 2
    F = np.fft.rfft(x, nf)
    r = np.fft.irfft(F * np.conj(F), nf)[:n]
    return r / r[0]


def prior(sec, t0_s, sigma):
    return np.exp(-0.5 * (np.log2(sec / t0_s) / sigma) ** 2)


def tps_pick(r, nframes, t0_s=0.5, sigma=0.9, tps2=True):
    """Ellis 2007's TPS / TPS2 argmax; returns the lag in frames."""
    lag = np.arange(1, nframes)
    sec = lag / RATE
    t = prior(sec, t0_s, sigma) * r[lag]
    if tps2:
        half = np.interp(2.0 * sec, lag, r[lag], left=0.0, right=0.0)
        t = t + 0.5 * prior(sec * 2, t0_s, sigma) * half
    i = int(np.argmax(t))
    return lag[i], t


def stats(v):
    v = np.array(v, dtype=float)
    v = v[np.isfinite(v)]
    return ("mean %6.2f  sd %5.2f  min %6.1f  p25 %6.1f  med %6.1f  "
            "p75 %6.1f  max %6.1f" % (v.mean(), v.std(), v.min(),
                                     np.percentile(v, 25), np.median(v),
                                     np.percentile(v, 75), v.max()))


def main():
    files = sorted(glob.glob(os.path.join(CACHE, "*.npy")))
    od = []
    for path in files:
        o = odf(np.load(path).astype(np.float64))
        if len(o) >= int(37.0 * RATE):
            od.append(o)
    print("%d real windows of 40 s\n" % len(od))

    # ---------------- 1. accuracy of the prior-weighted ACF -------------
    for win_s in (12.0,):
        n = int(win_s * RATE)
        bpms, rpks, floors, frac_pos = [], [], [], []
        for o in od:
            oo = o[:n]
            r = acf(oo)
            lag, _ = tps_pick(r, n)
            bpms.append(60.0 * RATE / lag)
            lag_min = int(60.0 * RATE / BPM_HI)
            lag_max = int(60.0 * RATE / BPM_LO)
            band = r[lag_min:lag_max + 1]
            rpks.append(r[lag])
            floors.append(np.median(band))
            frac_pos.append((band > 0).mean())
        print("== %g s window, prior centre 120 BPM, sigma 0.9 octaves ==" % win_s)
        print("  BPM            : %s" % stats(bpms))
        print("  ACF peak r     : %s" % stats(rpks))
        print("  ACF in-band floor (median r): %s" % stats(floors))
        print("  fraction of in-band lags with r > 0 : %.3f" %
              np.mean(frac_pos))
        print()

    # ---------------- 2. prior ablation -------------------------------
    print("== PRIOR ABLATION (12 s window) ==")
    n12 = int(12.0 * RATE)
    for t0_bpm in (90.0, 105.0, 120.0, 135.0, 150.0):
        picks = []
        for o in od:
            r = acf(o[:n12])
            lag, _ = tps_pick(r, n12, t0_s=60.0 / t0_bpm)
            picks.append(60.0 * RATE / lag)
        print("  prior centre %5.1f BPM -> %s" % (t0_bpm, stats(picks)))
    print()

    # ---------------- 3. window sweep --------------------------------
    print("== WINDOW SWEEP: the prior-weighted ACF's answer ==")
    print("  (each window's estimate vs that file's full-window estimate)")
    sweep = (4.0, 6.0, 8.0, 12.0, 16.0, 24.0, 36.0)
    for win_s in sweep:
        n = int(win_s * RATE)
        err, vals, refs = [], [], []
        for o in od:
            r40 = acf(o)
            lag40, _ = tps_pick(r40, len(o))
            ref = 60.0 * RATE / lag40
            r = acf(o[:n])
            lag, _ = tps_pick(r, n)
            est = 60.0 * RATE / lag
            refs.append(ref)
            err.append(est - ref)
            vals.append(est)
        e = np.array(err)
        # the Rayleigh cell in BPM at 128 BPM
        f0 = 128.0 / 60.0
        cell = (1.0 / win_s) / f0 * 128.0
        ratio = np.array(vals) / np.array(refs)
        oct2 = np.mean((np.abs(ratio - 2.0) < 0.08) |
                       (np.abs(ratio - 0.5) < 0.04))
        within4 = np.mean(np.abs(e) / np.array(refs) < 0.04)
        print("  %4.1f s  Rayleigh cell at 128 BPM = %5.2f BPM | "
              "error vs full: mean %+6.2f  sd %5.2f  |d| max %6.2f | "
              "estimate sd %5.2f | octave(2:1/1:2) %4.1f%% | within 4%% %4.1f%%" %
              (win_s, cell, e.mean(), e.std(), np.abs(e).max(),
               np.std(vals), 100 * oct2, 100 * within4))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Autocorrelation vs power spectrum on the REAL house-music ODF.

The literature says the autocorrelation of an onset envelope beats its power
spectrum for tempo estimation.  This measures by how much, on the actual ODF
the board produces (40-160 Hz band, rectified dB difference, 93.75 fps).

For every cached track and every 12 s window it computes

  - the spectrum: the same running-DFT bins the firmware uses, and
  - the autocorrelation of the same frames,

and reports for each the "crest": how far the winning peak stands above the
median of its own band, and how far it stands above the runner-up.

Then it sweeps the window length to show what the Rayleigh resolution costs in
BPM, using the 40 s window's autocorrelation peak as the reference tempo.

    python tools/ar_sim/_tempo13_contrast.py
"""

import glob
import json
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "_cache")

SR = 6000.0          # the cache's rate
HOP = 64             # 6000/64 = 93.75 frames per second, exactly
RATE = SR / HOP      # 93.75 fps
NFFT = 4096          # the firmware's grid

BPM_LO, BPM_HI = 90.0, 175.0
EPS = 1e-10

# The firmware's band-pass: 2nd-order highpass at 40 Hz + two 2nd-order
# lowpasses at 160 Hz, on the firmware's own transposed-direct-form-II.
BUTTER_Q = 1.0 / math.sqrt(2.0)


def rbj_hpf(f, fs, q):
    w0 = 2.0 * math.pi * f / fs
    alpha = math.sin(w0) / (2.0 * q)
    a0 = 1.0 + alpha
    return np.array([(1.0 + alpha) / a0, -(2.0 * math.cos(w0)) / a0,
                     (1.0 - alpha) / a0, 0.0, 1.0], dtype=np.float64)


def rbj_lpf(f, fs, q):
    w0 = 2.0 * math.pi * f / fs
    alpha = math.sin(w0) / (2.0 * q)
    a0 = 1.0 + alpha
    return np.array([(1.0 - alpha) / a0, -(2.0 * math.cos(w0)) / a0,
                     (1.0 + alpha) / a0, 0.0, 1.0], dtype=np.float64)


def biquad(x, c):
    """Transposed direct form II, as the firmware runs it (scipy lfilter)."""
    from scipy.signal import lfilter
    b0, b1, b2, a1, a2 = c
    return lfilter([b0, b1, b2], [1.0, a1, a2], x)


def odf(x):
    """The firmware's ODF: per-frame mean square in dB, rectified difference."""
    y = biquad(x, rbj_hpf(40.0, SR, BUTTER_Q))
    y = biquad(y, rbj_lpf(160.0, SR, BUTTER_Q))
    y = biquad(y, rbj_lpf(160.0, SR, BUTTER_Q))
    y = y[int(1.0 * SR):]          # let the band-pass settle
    n = (len(y) // HOP) * HOP
    frames = y[:n].reshape(-1, HOP)
    p = (frames.astype(np.float64) ** 2).mean(axis=1)
    db = 10.0 * np.log10(p + EPS)
    return np.maximum(np.diff(db, prepend=db[0]), 0.0)


def band_k():
    """The in-band FFT bins for the target tempo range."""
    k = np.arange(NFFT // 2 + 1)
    bpm = k * RATE / NFFT * 60.0
    return k[(bpm >= BPM_LO) & (bpm <= BPM_HI)]


def bin_bpm(k):
    return k * RATE / NFFT * 60.0


def spectrum_profile(o):
    """The spectrum of the ODF window, on the firmware's 4096 grid."""
    x = o - o.mean()
    P = np.abs(np.fft.rfft(x, NFFT))
    return P


def acf_profile(o):
    """The normalised autocorrelation of the ODF window, on the same grid.

    Zero-padded to NFFT so the lag axis is directly comparable to the
    frequency axis: r[lag] is the correlation at that lag in frames.
    """
    x = o - o.mean()
    n = len(x)
    # biased ACF via the FFT, on a zero-padded grid so lags are in frames
    nfft = 1
    while nfft < 2 * n:
        nfft *= 2
    F = np.fft.rfft(x, nfft)
    r = np.fft.irfft(F * np.conj(F), nfft)[:n]
    return r / r[0]


def peaks_in_band(vals, ks):
    """Local maxima of vals over the in-band ks, as (index, value)."""
    out = []
    for i in range(1, len(ks) - 1):
        if vals[ks[i]] > vals[ks[i - 1]] and vals[ks[i]] >= vals[ks[i + 1]]:
            out.append((i, vals[ks[i]]))
    return out


def crest(vals, ks):
    """(peak/median in dB, peak/runner-up in dB, n local maxima, peak value)."""
    band = vals[ks]
    pk = band.max()
    med = np.median(band)
    # runner-up: the highest local maximum that is not the peak bin
    pk_i = int(np.argmax(band))
    runner = 0.0
    for i in range(1, len(ks) - 1):
        if i == pk_i:
            continue
        if band[i] > band[i - 1] and band[i] >= band[i + 1]:
            runner = max(runner, band[i])
    if runner <= 0:
        runner = med
    pk_db = 10.0 * math.log10(pk / med) if med > 0 else 0.0
    ru_db = 20.0 * math.log10(pk / runner) if runner > 0 else 0.0
    return pk_db, ru_db, len(peaks_in_band(vals, ks)), pk


def main():
    meta = json.load(open(os.path.join(CACHE, "_meta.json")))
    names = sorted(meta.keys())
    files = sorted(glob.glob(os.path.join(CACHE, "*.npy")))
    ks = band_k()
    print("in-band bins %d..%d  (%.2f .. %.2f BPM)" %
          (ks[0], ks[-1], bin_bpm(ks[0]), bin_bpm(ks[-1])))

    spec_pk, spec_ru = [], []
    acf_pk, acf_ru = [], []
    spec_bpm, acf_bpm = [], []
    agree = 0
    n = 0
    win_s = 12.0
    want = int(win_s * RATE)

    for path in files:
            x = np.load(path).astype(np.float64)
            o = odf(x)
            if len(o) < want:
                continue
            # centre window
            o = o[:want]
            S = spectrum_profile(o)
            a_db, b_db, nloc, _ = crest(S, ks)
            pk_i = int(np.argmax(S[ks]))
            spec_bpm.append(bin_bpm(ks[pk_i]))

            # the ACF is computed on the same frames; its lag axis in frames
            r = acf_profile(o)
            # lag in frames -> BPM = 60 * RATE / lag
            lags = np.arange(1, want)
            lag_bpm = 60.0 * RATE / lags
            sel = (lag_bpm >= BPM_LO) & (lag_bpm <= BPM_HI)
            rl = lags[sel]
            rv = r[rl]
            pk = int(np.argmax(rv))
            acf_bpm.append(60.0 * RATE / rl[pk])
            # the ACF's floor: the median of the in-band ACF
            med = np.median(rv)
            pk_db = 20.0 * math.log10(rv[pk] / med) if med > 0 else 0.0
            # runner-up: highest local max that is not the winner
            runner = 0.0
            for i in range(1, len(rv) - 1):
                if i == pk:
                    continue
                if rv[i] > rv[i - 1] and rv[i] >= rv[i + 1]:
                    runner = max(runner, rv[i])
            ru_db = 20.0 * math.log10(rv[pk] / runner) if runner > 0 else 0.0

            spec_pk.append(a_db)
            spec_ru.append(b_db)
            acf_pk.append(pk_db)
            acf_ru.append(ru_db)
            if abs(spec_bpm[-1] - acf_bpm[-1]) < 4.0 * spec_bpm[-1] / 100.0:
                agree += 1
            n += 1

    def st(v):
        v = np.array(v)
        return "mean %6.2f  median %6.2f  min %6.2f  max %6.2f" % (
            v.mean(), np.median(v), v.min(), v.max())

    print("\n%d windows of %.1f s from %d tracks" % (n, win_s, len(names)))
    print("\n-- SPECTRUM (4096-pt DFT of the ODF window) --")
    print("  peak over band median : %s dB" % st(spec_pk))
    print("  peak over runner-up   : %s dB" % st(spec_ru))
    print("\n-- AUTOCORRELATION (same window) --")
    print("  peak over band median : %s dB" % st(acf_pk))
    print("  peak over runner-up   : %s dB" % st(acf_ru))
    print("\n-- AGREE --")
    print("  spectrum argmax within 4%% of ACF argmax: %d/%d = %.0f%%" %
          (agree, n, 100.0 * agree / n))
    print("  spectrum argmax BPM: mean %.1f median %.1f" %
          (np.mean(spec_bpm), np.median(spec_bpm)))
    print("  ACF      argmax BPM: mean %.1f median %.1f" %
          (np.mean(acf_bpm), np.median(acf_bpm)))


if __name__ == "__main__":
    main()

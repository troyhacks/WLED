#!/usr/bin/env python3
"""A tempo estimator built on the ODF's AUTOCORRELATION.

Why the autocorrelation and not the spectrum
--------------------------------------------

The spectral estimator in _tempo3_sim.py works on synthetic tracks and
fails on real house music: the in-band spectrum has ten local maxima spread
from 89 to 171 BPM, all within 14 dB, with no dominant peak, and the argmax
moves when the window length changes.

The autocorrelation of the SAME window does show the beat - lag 47 frames,
119.70 BPM, r = 0.217 against 0.116 at lag 48.  Why the difference?

Wiener-Khinchin says the autocorrelation IS the inverse transform of the
power spectrum, so they carry the same information.  But they do not
distribute it the same way, and the distribution is what a peak-picker
sees.

The power spectrum of a 12 s window has a true resolution of 1/12 Hz = 5 BPM
per cell.  Zero-padding to 4096 points makes the GRID 1.373 BPM but adds no
resolution - it interpolates.  So each genuine peak in the spectrum is a
~5 BPM wide lobe, and the rectangular window's -13 dB sidelobes put ripples
at -13 dB around it.  On a clean synthetic ODF the true lobe still wins.
On a dense, noisy real ODF, a dozen lobes of comparable height sit inside
the band and the argmax is whichever noise ripple happened to be tallest.

The autocorrelation does not have that problem, because the ODF's noise
floor is broadband in TIME, not in lag.  A broadband-noise ODF contributes a
roughly FLAT correlation floor to every lag, while a periodic component
contributes a peak.  The peak-to-floor ratio at the beat lag is therefore
set by how much of the ODF's variance is periodic, which is a much better
conditioned quantity than "which spectral lobe is tallest".

This is the standard reason the autocorrelation is the classical tempo
estimator (Dixon 2001) and why Ellis's dynamic-programming beat tracker
scores candidate periods rather than picking a spectral bin.

The metrical level
------------------

An autocorrelation of a periodic onset train peaks at the beat period AND at
its multiples (2x, 3x) and possibly its fractions.  Inside a 90-175 BPM band
the multiples of a 4/4 kick pattern fall outside the band (a 128 BPM track's
eighth-note grid is 256 BPM, its half-note grid 64 BPM), so the band does
most of the octave work by itself.  What remains is the 3:2 relation - a
dotted feel - which the simple-rational tie-break handles.

What this file provides
------------------------

    odf_of()      the onset detection function, with optional whitening
    acf()         the sliding autocorrelation over the lag band
    estimate()    several estimators, so they can be compared honestly
    score()       the score against librosa's independent tempo estimate

The FIRMWARE-SHAPED parts are: the sliding ACF update (O(lags) per frame,
not O(lags*N)), the whitening running mean, and the parabolic sub-lag
refinement.  The offline versions here are the same maths written plainly.
"""

import argparse
import glob
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _oracle_librosa                             # noqa: E402

SR = _cache.CACHE_RATE          # 6000
HOP = _cache.CACHE_HOP          # 64 -> 93.75 fps, the firmware's exact rate
RATE = SR / HOP                 # 93.75 frames per second

BPM_LO = 90.0
BPM_HI = 175.0

# The lag band.  BPM = 60*RATE/lag, so:
#   175 BPM -> lag 32.1
#    90 BPM -> lag 62.5
LAG_MIN = int(math.floor(60.0 * RATE / BPM_HI))     # 32
LAG_MAX = int(math.ceil(60.0 * RATE / BPM_LO))      # 63
N_LAGS = LAG_MAX - LAG_MIN + 1

WINDOW_S = 12.0
WIN = int(WINDOW_S * RATE)                          # 1125 frames

EPS = 1e-12


# --------------------------------------------------------------------------
# Filters
# --------------------------------------------------------------------------

def rbj_lpf(f, fs, q):
    w0 = 2.0 * math.pi * f / fs
    cw = math.cos(w0)
    sw = math.sin(w0)
    a = sw / (2.0 * q)
    b0 = (1.0 - cw) / 2.0
    b1 = 1.0 - cw
    b2 = (1.0 - cw) / 2.0
    a0 = 1.0 + a
    a1 = -2.0 * cw
    a2 = 1.0 - a
    return np.array([b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0])


def rbj_hpf(f, fs, q):
    w0 = 2.0 * math.pi * f / fs
    cw = math.cos(w0)
    sw = math.sin(w0)
    a = sw / (2.0 * q)
    b0 = (1.0 + cw) / 2.0
    b1 = -(1.0 + cw)
    b2 = (1.0 + cw) / 2.0
    a0 = 1.0 + a
    a1 = -2.0 * cw
    a2 = 1.0 - a
    return np.array([b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0])


def rbj_bpf(f, fs, q):
    w0 = 2.0 * math.pi * f / fs
    cw = math.cos(w0)
    sw = math.sin(w0)
    a = sw / (2.0 * q)
    b0 = q * a
    b1 = 0.0
    b2 = -q * a
    a0 = 1.0 + a
    a1 = -2.0 * cw
    a2 = 1.0 - a
    return np.array([b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0])


BUTTER_Q = 1.0 / math.sqrt(2.0)


def designs(fs=SR):
    """Band-pass designs for the onset detection function.

    'hp40_lp160x2' is the firmware's current default: a 40 Hz high-pass and
    a 160 Hz low-pass, each twice.  The rest are for comparison.
    """
    d = {}
    d["hp40_lp160x2"] = [rbj_hpf(40.0, fs, BUTTER_Q),
                         rbj_hpf(40.0, fs, BUTTER_Q),
                         rbj_lpf(160.0, fs, BUTTER_Q),
                         rbj_lpf(160.0, fs, BUTTER_Q)]
    d["hp40_lp160x1"] = [rbj_hpf(40.0, fs, BUTTER_Q),
                         rbj_lpf(160.0, fs, BUTTER_Q)]
    d["hp40_lp140x2"] = [rbj_hpf(40.0, fs, BUTTER_Q),
                         rbj_hpf(40.0, fs, BUTTER_Q),
                         rbj_lpf(140.0, fs, BUTTER_Q),
                         rbj_lpf(140.0, fs, BUTTER_Q)]
    d["hp30_lp180x2"] = [rbj_hpf(30.0, fs, BUTTER_Q),
                         rbj_hpf(30.0, fs, BUTTER_Q),
                         rbj_lpf(180.0, fs, BUTTER_Q),
                         rbj_lpf(180.0, fs, BUTTER_Q)]
    d["bpf85"] = [rbj_bpf(85.0, fs, 0.9)]
    d["bpf70"] = [rbj_bpf(70.0, fs, 0.9)]
    d["bpf55"] = [rbj_bpf(55.0, fs, 0.9)]
    return d


def apply_sections(x, sections):
    """The biquad cascade, run the way the firmware runs it.

    The firmware's transposed DF-II is
        y = b0*x + s1;  s1 = b1*x - a1*y + s2;  s2 = b2*x - a2*y
    with {b0,b1,b2,a1,a2} and a0 = 1.  scipy's lfilter is the equivalent
    direct form I,
        y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
    which is the same transfer function to within float rounding - and at
    40-160 Hz with Q = 0.707 the two forms differ in the noise floor by
    well under a hundredth of a dB.

    The point of using lfilter is that it is C-speed.  A hand-rolled
    frequency-domain convolution over 40 s of audio was taking minutes per
    estimator per window, which made every experiment a wait.
    """
    from scipy.signal import lfilter
    y = x.astype(np.float64)
    for c in sections:
        b0, b1, b2, a1, a2 = c
        y = lfilter([b0, b1, b2], [1.0, a1, a2], y)
    return y


# --------------------------------------------------------------------------
# The onset detection function
# --------------------------------------------------------------------------

def odf_of(x, design="hp40_lp160x2", whiten=0.0):
    """The ODF: band-passed energy in dB, rectified frame-to-frame difference.

    `whiten` is the length in seconds of a running mean subtracted before
    the rectification.  Dixon's adaptive thresholding: the running mean is a
    slow estimate of the local noise level, and subtracting it turns a
    DENSE envelope into a sparse one that responds to onsets rather than to
    level.  0 disables it.

    On a real house track the un-whitened ODF has mean ~0.99 and max ~83 -
    it rises almost every frame, so its autocorrelation floor is high.  The
    whitening is what pulls the beat peak back out of that floor.
    """
    sections = designs()[design]
    y = apply_sections(x, sections)
    n = (len(y) // HOP) * HOP
    if n == 0:
        return np.zeros(0)
    frames = y[:n].reshape(-1, HOP)
    p = (frames.astype(np.float64) ** 2).mean(axis=1)
    db = 10.0 * np.log10(p + EPS)
    if whiten > 0:
        k = max(1, int(whiten * RATE))
        c = np.cumsum(np.concatenate(([0.0], db)))
        lo = np.maximum(np.arange(len(db)) - k, 0)
        hi = np.arange(len(db))
        ma = (c[hi + 1] - c[lo]) / (hi - lo + 1)
        db = db - ma
    return np.maximum(np.diff(db, prepend=db[0]), 0.0)


# --------------------------------------------------------------------------
# ODF cache
#
# The filter cascade is the slow part and it does not depend on the
# estimator, so it is computed once per (window, design, whiten) and kept.
# Without this, scoring three estimators over 300 windows re-ran the same
# convolution 900 times.
# --------------------------------------------------------------------------

ODF_CACHE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_odfcache")


def odf_cached(key, x, design, whiten):
    tag = "%s__%s__%d" % (key, design, int(whiten * 100))
    path = os.path.join(ODF_CACHE_DIR, tag + ".npy")
    if os.path.exists(path):
        try:
            return np.load(path)
        except Exception:                        # noqa: BLE001
            pass
    o = odf_of(x, design, whiten)
    os.makedirs(ODF_CACHE_DIR, exist_ok=True)
    try:
        np.save(path, o)
    except Exception:                            # noqa: BLE001
        pass
    return o


# --------------------------------------------------------------------------
# The autocorrelation
# --------------------------------------------------------------------------

def acf_of(odf, win=WIN):
    """The autocorrelation of the ODF over the lag band.

    Returns (lags, r) where r[L] is the correlation at lag L (index 0 is
    LAG_MIN).  This is the OFFLINE version; the firmware's version is the
    same number maintained incrementally.

    The ODF is MEAN-REMOVED first.  The ODF is a rectified quantity with a
    large DC component, and the DC contributes a triangle to the
    autocorrelation that decays with lag - which would tilt the whole band
    and bias the peak towards short lags (fast tempos).  Removing the mean
    is the first thing Dixon's method does.
    """
    w = odf[-win:].astype(np.float64)
    if len(w) < win:
        return np.zeros(0), np.zeros(0)
    w = w - w.mean()
    r = np.zeros(N_LAGS)
    for i, L in enumerate(range(LAG_MIN, LAG_MAX + 1)):
        a = w[:len(w) - L]
        b = w[L:]
        r[i] = float(np.dot(a, b))
    return np.arange(LAG_MIN, LAG_MAX + 1), r


def acf_sliding(odf, win=WIN):
    """The SAME number, computed the way the firmware would.

    A sliding autocorrelation over a fixed window can be updated in O(lags)
    per frame: subtract the product that leaves the window, add the product
    that enters it.  Recomputing from scratch would be O(lags*N) per frame -
    36000 multiply-adds instead of 64.

    This is here to prove the two agree, and to measure how much the
    incremental form drifts.  The firmware would recompute exactly every
    few seconds to kill the drift.
    """
    n = len(odf)
    if n < win + LAG_MAX:
        return None
    ring = np.zeros(win + LAG_MAX + 1)
    acf = np.zeros(N_LAGS)
    out = []
    for t in range(n):
        ring[t % len(ring)] = odf[t] - (odf[:t].mean() if t > 0 else 0.0)
        if t >= win + LAG_MAX - 1:
            pass
        out.append(t)
    # the incremental version, done properly on a de-meaned stream
    w = odf.astype(np.float64)
    w = w - w.mean()
    hist = np.zeros(win + LAG_MAX + 2)
    for t in range(len(w)):
        hist[t % len(hist)] = w[t]
    acc = np.zeros(N_LAGS)
    series = []
    for t in range(len(w)):
        # the window is [t-win+1, t]; the newest sample enters at lag 0
        for i, L in enumerate(range(LAG_MIN, LAG_MAX + 1)):
            acc[i] += w[t] * w[t - L] if t >= L else 0.0
            if t >= win + L:
                acc[i] -= w[t - win] * w[t - win + L]
        if t >= win + LAG_MAX:
            series.append(acc.copy())
    return np.array(series)


def acf_at(odf, win, t_end):
    """The direct ACF of the window ending at frame t_end."""
    w = odf[t_end - win:t_end].astype(np.float64)
    w = w - w.mean()
    r = np.zeros(N_LAGS)
    for i, L in enumerate(range(LAG_MIN, LAG_MAX + 1)):
        r[i] = float(np.dot(w[:len(w) - L], w[L:]))
    return r


# --------------------------------------------------------------------------
# Peak picking
# --------------------------------------------------------------------------

def lag_bpm(L):
    return 60.0 * RATE / L


def refine_parabolic(lags, r, i):
    """Sub-lag peak location by a parabola through the peak and its
    neighbours.  The autocorrelation's peak is smooth in lag, so this is
    worth roughly a factor of ten in BPM resolution over the integer grid.
    """
    if i <= 0 or i >= len(r) - 1:
        return float(lags[i])
    y0, y1, y2 = r[i - 1], r[i], r[i + 1]
    denom = (y0 - 2.0 * y1 + y2)
    if denom == 0:
        return float(lags[i])
    delta = 0.5 * (y0 - y2) / denom
    if abs(delta) > 1.0:
        delta = 0.0
    return float(lags[i] + delta)


def simple_ratio(a, b, maxn=4, tol=0.01):
    """Is the ratio a/b a simple rational p/q?  Returns 'p/q' or None."""
    if b <= 0:
        return None
    r = a / b
    for q in range(1, maxn + 1):
        for p in range(q + 1, maxn + 1):
            if abs(r - p / q) < tol * (p / q):
                return "%d/%d" % (p, q)
    return None


def pick_peak(lags, r, tie_db=3.0, tie=True):
    """Pick the beat lag from the autocorrelation.

    The tallest peak in the band is the candidate.  Then the metrical
    check: if a SLOWER peak in the band is within `tie_db` of it and the
    two are a simple rational ratio apart, the slower one wins.  That is
    the 3:2 error (a dotted feel reading as 3 beats against 2) being
    pulled back to the true beat.

    A larger lag is a slower tempo, so we look for a larger lag.

    Returns the refined LAG (a fractional frame count), the peak's height,
    and the index of the chosen peak.
    """
    if len(r) == 0:
        return None, 0.0, None
    best = int(np.argmax(r))
    if not tie:
        return refine_parabolic(lags, r, best), float(r[best]), best

    # local maxima only
    peaks = []
    for i in range(len(r)):
        if i == 0 or i == len(r) - 1:
            continue
        if r[i] >= r[i - 1] and r[i] >= r[i + 1]:
            peaks.append(i)
    if not peaks:
        peaks = [best]

    top = max(peaks, key=lambda i: r[i])
    thr = r[top] / (10.0 ** (tie_db / 20.0))
    for i in peaks:
        if i <= top:
            continue
        if r[i] <= 0 or r[i] < thr:
            continue
        ratio = simple_ratio(lag_bpm(lags[top]), lag_bpm(lags[i]))
        if ratio:
            return refine_parabolic(lags, r, i), float(r[i]), i
    return refine_parabolic(lags, r, top), float(r[top]), top


def pick_peak_spectral_hybrid(lags, r, tie_db=3.0):
    """The autocorrelation peak, but with the peak's PROMINENCE used as the
    confidence: how far the peak stands above the band's median."""
    bpm, mag, i = pick_peak(lags, r, tie_db=tie_db)
    if bpm is None:
        return None, 0.0
    med = float(np.median(r))
    conf = 0.0 if med <= 0 else (mag - med) / med
    return bpm, conf


# --------------------------------------------------------------------------
# Estimators
# --------------------------------------------------------------------------

def estimate_acf(odf, design="hp40_lp160x2", whiten=0.0, win=WIN, tie=True):
    """The autocorrelation tempo estimate.

    Returns (bpm, confidence, peak_height).  The confidence is the peak's
    PROMINENCE over the band's median - how far the beat peak stands above
    the autocorrelation's floor.  That is the number that says "there is a
    beat here" as opposed to "the loudest thing in the band is here", and
    it is what the firmware needs in order to decide whether to publish.
    """
    lags, r = acf_of(odf, win)
    if len(r) == 0:
        return None, 0.0, 0.0
    lag, mag, i = pick_peak(lags, r, tie=tie)
    if lag is None or lag <= 0:
        return None, 0.0, 0.0
    bpm = lag_bpm(lag)
    med = float(np.median(r))
    conf = 0.0 if med <= 0 else (mag - med) / max(med, 1e-9)
    return bpm, conf, mag


def estimate_spectral(odf, win=WIN):
    """The old estimator, for comparison: the running DFT's offline
    equivalent, with the 3:2 tie-break."""
    w = odf[-win:].astype(np.float64)
    nfft = 4096
    P = np.abs(np.fft.rfft(w, nfft))
    ks = np.arange(int(BPM_LO / 60.0 * nfft / RATE),
                   int(math.ceil(BPM_HI / 60.0 * nfft / RATE)) + 1)
    mags = P[ks]
    bpms = ks * RATE / nfft * 60.0
    i = int(np.argmax(mags))
    thr = mags[i] / (10.0 ** (3.0 / 20.0))
    for j in range(i):
        if mags[j] < thr or mags[j] <= 0:
            continue
        if simple_ratio(bpms[i], bpms[j]):
            return float(bpms[j]), float(mags[j]), 0.0
    return float(bpms[i]), float(mags[i]), 0.0


def estimate_comb(odf, win=WIN, bpm_lo=90.0, bpm_hi=175.0, step=0.25):
    """A COMB-FILTER score over candidate tempos.

    For each candidate BPM, lay a comb of beat positions over the window
    and score it by the ODF's mean value AT those positions (as opposed to
    the ODF's mean everywhere, which is the same for every candidate).

    This is the 'does the music actually have beats where I say it does'
    question, asked directly.  It is more expensive than the ACF (O(cand *
    beats) instead of O(lags*N)) but it answers the metrical question
    directly instead of by proxy.
    """
    w = odf[-win:].astype(np.float64)
    if len(w) < win:
        return None, 0.0
    best = None
    best_score = -1e30
    nb = 0
    for bpm in np.arange(bpm_lo, bpm_hi + step, step):
        period = 60.0 * RATE / bpm
        if period < 2:
            continue
        nbeats = int((len(w) - 1) / period)
        if nbeats < 4:
            continue
        # try a few phases, take the best
        sc = -1e30
        for ph in np.arange(0.0, period, max(1.0, period / 8.0)):
            idx = ph + np.arange(nbeats) * period
            idx = idx[idx < len(w)].astype(int)
            if len(idx) < 4:
                continue
            s = float(np.mean(w[idx]))
            if s > sc:
                sc = s
        if sc > best_score:
            best_score = sc
            best = float(bpm)
            nb = nbeats
    if best is None:
        return None, 0.0
    return best, best_score


# --------------------------------------------------------------------------
# Scoring
# --------------------------------------------------------------------------

def octave_err(a, b):
    """The error in BPM, accounting for the octave relation.

    A tempo estimate that is exactly twice or half the truth is a
    METRICAL error, not a measurement error, and it is worth telling apart.
    """
    if a is None or b is None or b <= 0:
        return None
    e = abs(a - b)
    for k in (0.5, 2.0):
        if abs(a - b * k) < e:
            e = abs(a - b * k)
    return e


def score(keys, est_fn, label, verbose=False):
    """Run an estimator over every cached window and score it against
    librosa's independent tempo estimate."""
    rows = []
    for k in keys:
        o = _oracle_librosa.load(k)
        x = _cache.load(k)
        if o is None or x is None:
            continue
        truth = o["bpm"]
        if truth <= 0:
            continue
        try:
            bpm, conf = est_fn(k, x)
        except Exception as exc:                 # noqa: BLE001
            if verbose:
                print("  %-46s ERROR %s" % (k[:46], exc))
            continue
        if bpm is None:
            continue
        rows.append((k, bpm, truth, conf))
    if not rows:
        print("%-26s no rows" % label)
        return rows

    errs = np.array([abs(b - t) for _, b, t, _ in rows])
    oerrs = np.array([octave_err(b, t) or 0.0 for _, b, t, _ in rows])
    exact = sum(1 for e in errs if e <= 2.0)
    oct_ok = sum(1 for e in oerrs if e <= 2.0)
    med = float(np.median(errs))
    print("%-26s n=%3d  err<=2: %3d (%5.1f%%)  oct-tol: %3d (%5.1f%%)  med %6.2f  mean %6.2f  max %7.2f"
          % (label, len(rows), exact, 100.0 * exact / len(rows),
             oct_ok, 100.0 * oct_ok / len(rows), med, errs.mean(), errs.max()))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keys", default=None, help="limit to keys containing this")
    ap.add_argument("--design", default="hp40_lp160x2")
    ap.add_argument("--whiten", type=float, default=0.0)
    ap.add_argument("--window", type=float, default=WINDOW_S)
    ap.add_argument("--variants", action="store_true",
                    help="compare every estimator side by side")
    ap.add_argument("--show", action="store_true")
    args = ap.parse_args()

    keys = _cache.keys()
    if args.keys:
        keys = [k for k in keys if args.keys in k]
    win = int(args.window * RATE)
    print("%d windows   design %s   whiten %.2f s   window %.1f s"
          % (len(keys), args.design, args.whiten, args.window))

    def get_odf(k, x):
        return odf_cached(k, x, args.design, args.whiten)

    def spec_est(k, x):
        bpm, mag, _ = estimate_spectral(get_odf(k, x), win)
        return bpm, mag

    def acf_est(k, x):
        bpm, conf, _ = estimate_acf(get_odf(k, x), args.design, args.whiten, win)
        return bpm, conf

    def acf_notie(k, x):
        bpm, conf, _ = estimate_acf(get_odf(k, x), args.design, args.whiten, win,
                                    tie=False)
        return bpm, conf

    def comb_est(k, x):
        return estimate_comb(get_odf(k, x), win)

    if args.variants:
        rows = score(keys, spec_est, "spectral (old)")
        score(keys, acf_notie, "acf, no tie-break")
        rows = score(keys, acf_est, "acf")
        score(keys, comb_est, "comb filter")
        if args.show:
            for k, b, t, c in rows[:25]:
                print("  %-50s  est %7.2f  lib %7.2f  d %7.2f"
                      % (k[:48], b, t, b - t))
    else:
        rows = score(keys, spec_est, "spectral (old)")
        rows = score(keys, acf_est, "acf")
        if args.show:
            for k, b, t, c in rows[:25]:
                print("  %-50s  est %7.2f  lib %7.2f  d %7.2f"
                      % (k[:48], b, t, b - t))


if __name__ == "__main__":
    sys.exit(main())

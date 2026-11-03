#!/usr/bin/env python3
"""Second tempo experiment: the ODF's BAND WEIGHTING decides the tempo.

The first experiment (tools/ar_sim/_tempo_sim.py) found the catalogue tempo as
the tallest ACF peak, and failed on two counts:

  * the band sweep got WORSE as the band narrowed (26 wrong at 90-175,
    40 wrong at 120-140). A tempo prior would predict the opposite, so the
    failures are NOT octave errors - they are the wrong PEAK being chosen.

  * the ACF dumps showed competing peaks at L, 1.5L, 0.75L... - ratios that
    are not octaves.

What those have in common: the 16th-note hat grid puts a peak at every
multiple of the grid period, and several of those land inside any sane band.
At 150 BPM the grid is 8ths, so the in-band candidates are 37.5 frames
(150 BPM) and 56.25 frames (100 BPM) - a 3:2 pair, not an octave pair.

The resolution is that the 3:2 candidate is a WEAKER periodicity of the KICK
than the beat is. The kick sits on the beat; the dotted-quarter grid puts
some kicks off the beat, so they do not add in phase. The ACF cannot see
that (it correlates the whole onset train, hats included, which is uniform
on the grid). A DFT CAN: it sums the kick-band onsets COMPLEX, so anything
that is not phase-coherent cancels.

So this script tests a two-ODF estimator:

    ODF_full  - every percussive onset, hats included. Establishes the grid.
    ODF_low   - the kick band only. Its spectrum has a peak at the BEAT.

and scores a candidate period L by the coherence of ODF_low at 1/L:

    score(L) = |sum_t odf_low[t] . exp(-j 2 pi t / L)| / sum_t odf_low[t]

which is maximal exactly when the kick onsets are all in phase at that
period, and near zero for a period they do not share.

    python tools/ar_sim/_tempo2_sim.py
    python tools/ar_sim/_tempo2_sim.py --skip 0.25
"""

import argparse
import math
import sys

import numpy as np

SR = 48000
HOP = 512
RATE = SR / HOP                       # 93.75 frames per second

ISO_BANDS = [10 ** ((13 + k) * 0.1) for k in range(31)]

# The kick band, in Hz. The AR's band 8 is 50.12 Hz and band 17 is 141.3 Hz,
# so this is "the eight bands that carry the kick".
KICK_LO, KICK_HI = 40.0, 180.0


# --------------------------------------------------------------------------
# Synthetic percussion - the same generators as _tempo_sim.py, kept separate
# so that file stays the record of the first experiment.
# --------------------------------------------------------------------------


def _env(n, decay_ms):
    return np.exp(-np.arange(n) / (decay_ms * 1e-3 * SR))


def hit_kick(n=4096):
    t = np.arange(n) / SR
    f = 52 + 90 * np.exp(-t / 0.020)
    phase = 2 * np.pi * np.cumsum(f) / SR
    body = np.sin(phase) * _env(n, 120)
    click = np.random.randn(n) * np.exp(-np.arange(n) / (2 * 1e-3 * SR)) * 0.25
    return body + click


def hit_snare(n=4096):
    noise = np.random.randn(n)
    noise = np.diff(np.concatenate(([0.0], noise)))
    tone = np.sin(2 * np.pi * 185 * np.arange(n) / SR) * 0.4
    return (noise * 0.6 + tone) * _env(n, 110)


def hit_hat(n=2048):
    noise = np.diff(np.concatenate(([0.0], np.random.randn(n))))
    return noise * _env(n, 28) * 0.35


def hit_clap(n=4096):
    noise = np.random.randn(n)
    noise = np.diff(np.concatenate(([0.0], noise)))
    burst = np.zeros(n)
    for off in (0, 220, 440):
        burst[off:off + 260] += np.random.randn(260) * 0.6
    tail = noise * np.exp(-np.arange(n) / (0.09 * SR)) * 0.35
    return (burst + tail) * 0.5


def hit_bass(n=8192, f=55.0):
    t = np.arange(n) / SR
    return np.sin(2 * np.pi * f * t) * _env(n, 260) * 0.5


def _place(buf, hit, at, gain=1.0):
    i = int(at * SR)
    if i < 0:
        i = 0
    if i >= len(buf):
        return
    seg = hit[: min(len(hit), len(buf) - i)] * gain
    buf[i:i + len(seg)] += seg


STYLES = ["house", "trap", "dnb", "hiphop", "garage", "hardstyle"]


def _seed(style, bpm):
    return (sum(ord(c) for c in style) * 131 + int(bpm) * 17) % 9973


def make_track(bpm, seconds=16.0, style="house", seed=1, human=0.0, noise=0.0):
    """A synthetic track with a known catalogue tempo.

    The styles differ in WHERE the onsets fall, because that is the whole
    question. A four-on-the-floor kick makes the fundamental BE the catalogue
    tempo; a half-time drum makes the fundamental the half-time tempo, and
    the catalogue tempo is then a harmonic of the finest grid instead.

    `human` adds timing jitter in ms (1 sigma) and `noise` adds a broadband
    noise floor - real tracks have both, and a rule that only works on a
    perfectly quantised grid is not a rule.
    """
    np.random.seed(seed)
    n = int(seconds * SR)
    buf = np.zeros(n + SR)
    beat = 60.0 / bpm
    nbeats = int(seconds / beat)
    jit = (lambda: np.random.randn() * human * 1e-3) if human else (lambda: 0.0)
    vel = (lambda: 1.0 + np.random.randn() * 0.12) if human else (lambda: 1.0)

    kick, snare, hat, clap = hit_kick(), hit_snare(), hit_hat(), hit_clap()

    def p(hit, at, gain=1.0):
        """Place a hit, with the human timing and velocity jitter."""
        _place(buf, hit, at + jit(), max(0.05, gain * vel()))

    for b in range(nbeats):
        t = b * beat
        inbar = b % 4

        if style == "house":
            p(kick, t, 1.0)
            if inbar in (1, 3):
                p(clap, t, 0.55)
            if b % 2 == 0:
                p(hit_bass(f=55.0), t, 0.5)
            for h in range(2):
                p(hat, t + h * beat / 2, 0.5)

        elif style == "trap":
            if b % 2 == 0:
                p(kick, t, 1.0)
                p(hit_bass(f=45.0), t, 0.55)
            if inbar == 2:
                p(snare, t, 0.8)
            for h in range(2):
                p(hat, t + h * beat / 2, 0.6)

        elif style == "dnb":
            if inbar in (0, 2):
                p(kick, t, 0.95)
                p(hit_bass(f=48.0), t, 0.6)
            if inbar in (1, 3):
                p(snare, t, 0.85)
            for h in range(4):
                p(hat, t + h * beat / 4, 0.45)

        elif style == "hiphop":
            if inbar in (0, 2):
                p(kick, t, 0.95)
            if inbar in (1, 3):
                p(snare, t, 0.85)
            for h in range(2):
                p(hat, t + h * beat / 2, 0.5)

        elif style == "garage":
            if inbar in (0, 1):
                p(kick, t, 0.9)
            if inbar == 2:
                p(snare, t, 0.8)
            p(hat, t + beat * 0.66, 0.55)
            p(hat, t + beat / 2, 0.45)

        elif style == "hardstyle":
            p(kick, t, 1.0)
            if inbar in (1, 3):
                p(clap, t, 0.5)
            for h in range(4):
                p(hat, t + h * beat / 4, 0.5)

        else:
            raise ValueError(style)

    buf = buf[:n]
    peak = np.max(np.abs(buf))
    if peak > 0:
        buf = buf / peak * 0.7
    if noise > 0:
        buf = buf + np.random.randn(n) * noise
    return buf


# --------------------------------------------------------------------------
# Onset detection functions
# --------------------------------------------------------------------------


def band_split(x, bands):
    """Filter the whole track into (lo, hi) bands. Whole-track filtering, so
    a 40 Hz band is resolved properly - a 512-sample FFT would not be."""
    n = len(x)
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(n, 1.0 / SR)
    out = []
    for lo, hi in bands:
        m = ((f >= lo) & (f < hi)).astype(np.float64)
        out.append(np.fft.irfft(X * m, n=n))
    return out


def odf_energy(y, hop=HOP):
    """Energy envelope in dB, half-wave rectified difference.

    This is the ODF the firmware can afford: the frame's 512 samples are
    already there, and a sum of squares is 512 multiply-adds."""
    n = (len(y) // hop) * hop
    fr = y[:n].reshape(-1, hop)
    p = (fr * fr).mean(axis=1)
    db = 10.0 * np.log10(p + 1e-12)
    return np.maximum(np.diff(db, prepend=db[0]), 0.0)


def odf_multiband(x, hop=HOP):
    """Log-domain spectral flux over the AR's 31 bands. The ODF the AR could
    get for free. Kept as the reference point."""
    n = (len(x) // hop) * hop
    frames = x[:n].reshape(-1, hop)
    w = np.hanning(hop)
    F = np.fft.rfft(frames * w, axis=1)
    P = np.abs(F) ** 2
    df = SR / hop
    freqs = np.fft.rfftfreq(hop, 1.0 / SR)
    out = np.zeros((len(frames), len(ISO_BANDS)))
    for k, fc in enumerate(ISO_BANDS):
        lo = fc / 2 ** (1 / 6.0)
        hi = fc * 2 ** (1 / 6.0)
        sel = (freqs >= lo) & (freqs < hi)
        if not np.any(sel):
            sel = np.arange(len(freqs)) == int(np.argmin(np.abs(freqs - fc)))
        out[:, k] = P[:, sel].sum(axis=1)
    out = np.maximum(out, 1e-20)
    L = np.log(out)
    flux = np.maximum(np.diff(L, axis=0, prepend=L[:1]), 0.0)
    return flux.sum(axis=1)


def make_odfs(x):
    """The two ODFs the estimator needs."""
    low, mid, high = band_split(x, [(KICK_LO, KICK_HI), (KICK_HI, 2000.0),
                                    (2000.0, 22000.0)])
    return {
        "full": odf_energy(x),
        "low": odf_energy(low),
        "mid": odf_energy(mid),
        "high": odf_energy(high),
        "multiband": odf_multiband(x),
    }


# --------------------------------------------------------------------------
# Estimators
# --------------------------------------------------------------------------


def spectrum(odf, rate, window_s, zoom=4, kind="hann"):
    """The magnitude spectrum of the onset envelope, zero-padded for a fine
    frequency grid.

    `kind` is the window shape. A Hann window has better sidelobes, but a
    RECTANGULAR window is the one an incremental (running) DFT can use: the
    sliding sum over a fixed length can be updated in O(1) per sample, while
    a Hann window cannot. The firmware wants the rectangular one."""
    n = int(window_s * rate)
    if len(odf) < n:
        return None, None
    x = odf[-n:].astype(np.float64)
    x = x - x.mean()
    if kind == "hann":
        x = x * np.hanning(n)
    nfft = 1 << int(math.ceil(math.log2(zoom * len(x))))
    X = np.fft.rfft(x, nfft)
    P = np.abs(X)
    freqs = np.fft.rfftfreq(nfft, 1.0 / rate)
    return freqs, P


def spec_at(freqs, P, bpm):
    """The magnitude at a BPM, linearly interpolated between bins."""
    f = bpm / 60.0
    if freqs is None or f < freqs[0] or f > freqs[-1]:
        return 0.0
    k = int(np.searchsorted(freqs, f))
    if k == 0:
        return float(P[0])
    if k >= len(freqs):
        return float(P[-1])
    t = (f - freqs[k - 1]) / (freqs[k] - freqs[k - 1])
    return float(P[k - 1] * (1 - t) + P[k] * t)


def peak_in(freqs, P, bpm_min, bpm_max):
    """The tallest spectral peak in a band, with parabolic refinement."""
    if freqs is None:
        return None, 0.0
    sel = np.where((freqs >= bpm_min / 60.0) & (freqs <= bpm_max / 60.0))[0]
    if len(sel) == 0:
        return None, 0.0
    k = int(sel[np.argmax(P[sel])])
    bpm = freqs[k] * 60.0
    if 0 < k < len(P) - 1:
        a, b, c = np.log(P[k - 1] + 1e-30), np.log(P[k] + 1e-30), np.log(P[k + 1] + 1e-30)
        den = a - 2 * b + c
        if abs(den) > 1e-12:
            d = 0.5 * (a - c) / den
            if abs(d) < 1.0:
                bpm += d * (freqs[1] - freqs[0]) * 60.0
    return bpm, float(P[k])


def spec_peak(odf, rate, bpm_min, bpm_max, window_s, whiten=False):
    """The tallest spectral peak of the onset envelope inside the band.

    The onset envelope is periodic, so its spectrum has peaks at the tempo
    and its harmonics; the tallest in-band peak is the estimate."""
    n = int(window_s * rate)
    if len(odf) < n:
        return None, 0.0
    x = odf[-n:].astype(np.float64)
    x = x - x.mean()
    x = x * np.hanning(n)
    nfft = 1 << int(math.ceil(math.log2(4 * len(x))))
    X = np.fft.rfft(x, nfft)
    P = np.abs(X)
    if whiten:
        # flatten the 1/f shape so the peak search is not dominated by the
        # lowest frequencies
        k = 9
        kern = np.ones(k) / k
        sm = np.convolve(P, kern, mode="same")
        P = P / np.maximum(sm, 1e-12)
    freqs = np.fft.rfftfreq(nfft, 1.0 / rate)
    sel = np.where((freqs >= bpm_min / 60.0) & (freqs <= bpm_max / 60.0))[0]
    if len(sel) == 0:
        return None, 0.0
    k = int(sel[np.argmax(P[sel])])
    bpm = freqs[k] * 60.0
    if 0 < k < len(P) - 1:
        a, b, c = np.log(P[k - 1] + 1e-30), np.log(P[k] + 1e-30), np.log(P[k + 1] + 1e-30)
        den = a - 2 * b + c
        if abs(den) > 1e-12:
            d = 0.5 * (a - c) / den
            if abs(d) < 1.0:
                bpm += d * (freqs[1] - freqs[0]) * 60.0
    return bpm, float(P[k])


def coherence_score(odf, period_frames):
    """How phase-coherent the onsets are at a given period.

    This is the discriminator the ACF cannot make. A period the onsets do
    NOT share (the 3:2 dotted quarter) sums to near zero; the period they do
    share (the beat) sums to nearly the whole energy."""
    n = len(odf)
    t = np.arange(n)
    z = np.sum(odf * np.exp(-2j * np.pi * t / period_frames))
    return abs(z) / max(np.sum(odf), 1e-20)


def coherent_peak(odf_low, odf_full, rate, bpm_min, bpm_max, window_s,
                  n_cand=48):
    """Pick the in-band period whose kick-band onsets are the most coherent,
    among the periods the FULL onset envelope actually supports.

    The candidate set comes from the full ODF (the grid); the coherence test
    on the kick band chooses between them."""
    n = int(window_s * rate)
    if len(odf_low) < n or len(odf_full) < n:
        return None, 0.0
    lo = odf_low[-n:].astype(np.float64)
    fl = odf_full[-n:].astype(np.float64)

    # the grid: spectral peaks of the full ODF
    grid = []
    bpm, mag = spec_peak(fl, rate, bpm_min, bpm_max, window_s)
    if bpm:
        grid.append(bpm)
    # and the harmonics of the grid, which is where the catalogue tempo
    # lives when the grid is finer than the beat
    for k in (2, 3, 4, 1.0 / 2, 1.0 / 3):
        b2, _ = spec_peak(fl, rate, bpm_min * k, bpm_max * k, window_s)
        if b2 and bpm_min <= b2 <= bpm_max:
            grid.append(b2)
    if not grid:
        return None, 0.0

    # score every candidate period by the kick-band coherence, but only the
    # ones the full envelope supports
    best = None
    best_s = -1.0
    for cand in grid:
        # a small neighbourhood, so the peak can be refined
        for j in range(-2, 3):
            b = cand * (1.0 + j * 0.004)
            if not (bpm_min <= b <= bpm_max):
                continue
            per = 60.0 * rate / b
            s = coherence_score(lo, per)
            if s > best_s:
                best_s = s
                best = b
    return best, best_s


def acf_peak(odf, rate, bpm_min, bpm_max, window_s):
    """The first experiment's estimator, kept for comparison."""
    n = int(window_s * rate)
    if len(odf) < n:
        return None, 0.0
    x = odf[-n:]
    x = x - x.mean()
    x = x * np.hanning(len(x))
    nfft = 1 << int(math.ceil(math.log2(2 * len(x))))
    X = np.fft.rfft(x, nfft)
    r = np.fft.irfft(np.abs(X) ** 2, nfft)[: len(x)]
    r /= max(r[0], 1e-20)
    lo = max(1, int(60.0 * rate / bpm_max))
    hi = min(len(r) - 2, int(60.0 * rate / bpm_min))
    if hi <= lo:
        return None, 0.0
    w = r[lo:hi + 1]
    k = int(np.argmax(w))
    lag = lo + k
    if 0 < k < len(w) - 1:
        a, b, c = w[k - 1], w[k], w[k + 1]
        den = a - 2 * b + c
        if abs(den) > 1e-12:
            d = 0.5 * (a - c) / den
            if abs(d) < 1.0:
                lag += d
    return 60.0 * rate / lag, float(w[k])


# --------------------------------------------------------------------------
# The experiment
# --------------------------------------------------------------------------

ESTIMATORS = ("acf_full", "acf_multiband", "spec_full", "spec_low",
              "spec_full_whiten", "coherent", "spec_prod", "spec_low_prior",
              "spec_low_lowest", "R1_argmax", "R2_3_2", "R3_grid",
              "R4_grid_low", "R5_prior")


# --------------------------------------------------------------------------
# Selection rules: given the two spectra, which in-band frequency is the tempo
# --------------------------------------------------------------------------


def _peaks(f, P, lo_bpm, hi_bpm):
    """Local maxima of the spectrum in a band, as (bpm, magnitude) pairs."""
    sel = np.where((f >= lo_bpm / 60.0) & (f <= hi_bpm / 60.0))[0]
    out = []
    for k in sel:
        if 0 < k < len(P) - 1 and P[k] >= P[k - 1] and P[k] >= P[k + 1]:
            out.append((f[k] * 60.0, float(P[k])))
    return out


def zoom_sweep(x, band, window_s, rate, kind, human, noise, style, bpm, seed):
    """How fine must the frequency grid be? The firmware's cost model turns on
    this: a bigger FFT or a bigger set of running DFT accumulators."""
    odfs = make_odfs(x)
    odf = odfs["low"]
    import numpy as _np
    n = int(window_s * rate)
    xw = odf[-n:].astype(_np.float64)
    xw = xw - xw.mean()
    if kind == "hann":
        xw = xw * _np.hanning(n)
    out = []
    for nfft in (1024, 2048, 4096, 8192, 16384):
        X = _np.fft.rfft(xw, nfft)
        P = _np.abs(X)
        fr = _np.fft.rfftfreq(nfft, 1.0 / rate)
        est, _ = peak_in(fr, P, band[0], band[1])
        # the 3:2 tie-break on the same grid
        best, mag = peak_in(fr, P, band[0], band[1])
        if best is not None:
            for bb, mm in _peaks(fr, P, band[0], band[1]):
                if bb >= best:
                    continue
                if mm <= 0 or mm < mag / (10 ** (3.0 / 20.0)):
                    continue
                r = best / bb
                for q in range(1, 5):
                    for pp in range(q + 1, 5):
                        if abs(r - pp / q) < 0.01 * (pp / q):
                            best = bb
                            break
        spacing_bpm = (rate / nfft) * 60.0
        out.append((nfft, spacing_bpm, best, abs(best - bpm) if best else 999.0))
    return out


def rule_argmax(odfs, band, window_s, rate, kind="hann", **kw):
    """R1: the tallest in-band peak of the kick band."""
    fk, Pk = spectrum(odfs["low"], rate, window_s, kind=kind)
    return peak_in(fk, Pk, band[0], band[1])


def rule_3_2(odfs, band, window_s, rate, tol_db=3.0, max_ratio=4, kind="hann", **kw):
    """R2: R1, but a lower peak that forms a simple rational ratio with the
    winner and is within tol_db of it wins instead.

    The catalogue tempo is the BEAT; the 3:2 rival is the dotted reading of
    the same pattern, and it is only a rival when the two are nearly as
    strong."""
    fk, Pk = spectrum(odfs["low"], rate, window_s, kind=kind)
    best, mag = peak_in(fk, Pk, band[0], band[1])
    if best is None:
        return None, 0.0
    for b, m in _peaks(fk, Pk, band[0], band[1]):
        if b >= best:
            continue
        if m <= 0 or m < mag / (10 ** (tol_db / 20.0)):
            continue
        r = best / b
        for q in range(1, max_ratio + 1):
            for p in range(q + 1, max_ratio + 1):
                if abs(r - p / q) < 0.01 * (p / q):
                    return b, m
    return best, mag


def rule_grid(odfs, band, window_s, rate, lo=1.6, hi=3.0, **kw):
    """R3: the percussive GRID is the finest subdivision the track uses; the
    tempo is the grid divided by an integer.

    The grid is the highest significant peak of the full onset envelope's
    spectrum, above the search band."""
    fl, Pl = spectrum(odfs["full"], rate, window_s)
    fk, Pk = spectrum(odfs["low"], rate, window_s)
    if fl is None:
        return None, 0.0
    cand = _peaks(fl, Pl, band[0] * lo, band[1] * hi)
    if not cand:
        return rule_argmax(odfs, band, window_s, rate)
    grid = max(cand, key=lambda p: p[1])[0]
    best, bestm = None, -1.0
    k = 2
    while grid / k >= band[0]:
        b = grid / k
        if b <= band[1]:
            m = spec_at(fk, Pk, b)
            if m > bestm:
                bestm, best = m, b
        k += 1
    if best is None:
        return rule_argmax(odfs, band, window_s, rate)
    # refine
    for b in np.arange(best - 0.5, best + 0.51, 0.02):
        if band[0] <= b <= band[1] and spec_at(fk, Pk, b) > bestm:
            bestm, best = spec_at(fk, Pk, b), b
    return best, bestm


def rule_grid_low(odfs, band, window_s, rate, lo=1.6, hi=3.0, **kw):
    """R4: the grid rule, but the grid is taken from the KICK band too - the
    kick band's highest significant peak is the finest grid the low end
    articulates."""
    fk, Pk = spectrum(odfs["low"], rate, window_s)
    fl, Pl = spectrum(odfs["full"], rate, window_s)
    if fk is None:
        return None, 0.0
    cand = _peaks(fk, Pk, band[0] * lo, band[1] * hi)
    if not cand:
        return rule_argmax(odfs, band, window_s, rate)
    grid = max(cand, key=lambda p: p[1])[0]
    best, bestm = None, -1.0
    k = 2
    while grid / k >= band[0]:
        b = grid / k
        if b <= band[1] and spec_at(fk, Pk, b) > bestm:
            bestm, best = spec_at(fk, Pk, b), b
        k += 1
    if best is None:
        return rule_argmax(odfs, band, window_s, rate)
    for b in np.arange(best - 0.5, best + 0.51, 0.02):
        if band[0] <= b <= band[1] and spec_at(fk, Pk, b) > bestm:
            bestm, best = spec_at(fk, Pk, b), b
    return best, bestm


def rule_prior(odfs, band, window_s, rate, centre=120.0, sigma=0.4, **kw):
    """R5: the tallest in-band peak, weighted by a log-normal tempo prior."""
    fk, Pk = spectrum(odfs["low"], rate, window_s)
    best, bests = None, -1.0
    for b in np.arange(band[0], band[1] + 0.05, 0.05):
        s = spec_at(fk, Pk, b) * lognormal_prior(b, centre, sigma)
        if s > bests:
            bests, best = s, b
    return best, bests


def rule_grid_and_32(odfs, band, window_s, rate, **kw):
    """R6: the grid rule, then the 3:2 tie-break on top."""
    b, m = rule_grid(odfs, band, window_s, rate)
    if b is None:
        return None, 0.0
    fk, Pk = spectrum(odfs["low"], rate, window_s, kind=kind)
    best, mag = peak_in(fk, Pk, band[0], band[1])
    return rule_3_2(odfs, band, window_s, rate)


RULES = {
    "R1_argmax": rule_argmax,
    "R2_3_2": rule_3_2,
    "R3_grid": rule_grid,
    "R4_grid_low": rule_grid_low,
    "R5_prior": rule_prior,
}


def scan_band(odfs, band, window_s, rate, alpha=1.0, prior=None, step=0.25):
    """Score every BPM in the band and return the best.

    score = low_mag * full_mag**alpha * prior

    The low band carries the kick, the full band carries the percussive grid.
    A tempo that only one of them supports is a harmonic of something else.
    """
    fl, Pl = spectrum(odfs["full"], rate, window_s)
    fk, Pk = spectrum(odfs["low"], rate, window_s)
    best, best_s = None, -1.0
    for b in np.arange(band[0], band[1] + step, step):
        s = spec_at(fk, Pk, b) * (spec_at(fl, Pl, b) ** alpha)
        if prior is not None:
            s *= prior(b)
        if s > best_s:
            best_s = s
            best = b
    # refine on a 0.02 BPM grid around the winner
    if best is not None:
        for b in np.arange(best - step, best + step + 0.001, 0.02):
            if not (band[0] <= b <= band[1]):
                continue
            s = spec_at(fk, Pk, b) * (spec_at(fl, Pl, b) ** alpha)
            if prior is not None:
                s *= prior(b)
            if s > best_s:
                best_s = s
                best = b
    return best, best_s


def lognormal_prior(bpm, centre=120.0, sigma=0.55):
    """A broad log-normal tempo prior, in octaves."""
    x = math.log(bpm / centre)
    return math.exp(-0.5 * (x / sigma) ** 2)


def inject_skips(odf, rate, skip=0.25, seed=3):
    """Blank a fraction of the onsets - the 'skipped beats' failure mode.

    Troy's note from the field: a beat that produces no onset must not shift
    the reported phase or make the estimate jump. Here whole BEAT blocks of
    the onset envelope are zeroed."""
    rng = np.random.RandomState(seed)
    out = odf.copy()
    beat = int(rate * 60.0 / 128.0)
    for i in range(len(out) // beat):
        if rng.rand() < skip:
            out[i * beat:(i + 1) * beat] = 0.0
    return out


def lowest_in_band_harmonic(odfs, band, window_s, rate):
    """The lowest in-band spectral peak of the kick band.

    The kick pattern's fundamental may be BELOW the band (a half-time drum),
    in which case the tempo is a harmonic of it - and the catalogue tempo is
    the LOWEST such harmonic, not the loudest."""
    fk, Pk = spectrum(odfs["low"], rate, window_s)
    if fk is None:
        return None, 0.0
    sel = np.where((fk >= band[0] / 60.0) & (fk <= band[1] / 60.0))[0]
    if len(sel) == 0:
        return None, 0.0
    # local maxima only
    peaks = []
    for k in sel:
        if 0 < k < len(Pk) - 1 and Pk[k] >= Pk[k - 1] and Pk[k] >= Pk[k + 1]:
            peaks.append((fk[k] * 60.0, float(Pk[k])))
    if not peaks:
        k = int(sel[np.argmax(Pk[sel])])
        return fk[k] * 60.0, float(Pk[k])
    tallest = max(p[1] for p in peaks)
    # the lowest peak that is within 12 dB of the tallest is a real harmonic,
    # not a sidelobe
    real = [p for p in peaks if p[1] > tallest / 4.0]
    real.sort()
    return real[0][0], real[0][1]


def estimate(x, est, band, window_s, skip=None, kind="hann"):
    odfs = make_odfs(x)
    rate = RATE
    if skip:
        odfs["low"] = inject_skips(odfs["low"], rate, skip)
        odfs["full"] = inject_skips(odfs["full"], rate, skip)
    if est == "acf_full":
        return acf_peak(odfs["full"], rate, band[0], band[1], window_s)
    if est == "acf_multiband":
        return acf_peak(odfs["multiband"], rate, band[0], band[1], window_s)
    if est == "spec_full":
        return spec_peak(odfs["full"], rate, band[0], band[1], window_s)
    if est == "spec_low":
        return spec_peak(odfs["low"], rate, band[0], band[1], window_s)
    if est == "spec_full_whiten":
        return spec_peak(odfs["full"], rate, band[0], band[1], window_s, whiten=True)
    if est == "coherent":
        return coherent_peak(odfs["low"], odfs["full"], rate,
                             band[0], band[1], window_s)
    if est == "spec_prod":
        return scan_band(odfs, band, window_s, rate, alpha=1.0)
    if est == "spec_low_prior":
        return scan_band(odfs, band, window_s, rate, alpha=0.0,
                         prior=lambda b: lognormal_prior(b))
    if est == "spec_low_lowest":
        return lowest_in_band_harmonic(odfs, band, window_s, rate)
    if est in RULES:
        return RULES[est](odfs, band, window_s, rate, kind=kind)
    raise ValueError(est)


def dump(odfs, band, window_s, rate, truth):
    """What the two spectra actually look like in the band."""
    fl, Pl = spectrum(odfs["full"], rate, window_s)
    fk, Pk = spectrum(odfs["low"], rate, window_s)
    for name, f, P in (("low ", fk, Pk), ("full", fl, Pl)):
        sel = np.where((f >= band[0] / 60.0) & (f <= band[1] / 60.0))[0]
        peaks = []
        for k in sel:
            if 0 < k < len(P) - 1 and P[k] >= P[k - 1] and P[k] >= P[k + 1]:
                peaks.append((f[k] * 60.0, float(P[k])))
        peaks.sort(key=lambda p: -p[1])
        top = "  ".join("%6.2f(%5.0f)" % (b, v) for b, v in peaks[:5])
        print("   %s peaks: %s" % (name, top))
    print("   truth: %.2f   low@truth %.0f  full@truth %.0f" % (
        truth, spec_at(fk, Pk, truth), spec_at(fl, Pl, truth)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--style", default=None)
    ap.add_argument("--est", default="spec_low", choices=ESTIMATORS)
    ap.add_argument("--band", default="90-175")
    ap.add_argument("--window", type=float, default=12.0)
    ap.add_argument("--bpms", default="90,100,120,128,140,150,160,174")
    ap.add_argument("--human", type=float, default=0.0,
                    help="timing jitter, ms (1 sigma)")
    ap.add_argument("--noise", type=float, default=0.0,
                    help="broadband noise amplitude")
    ap.add_argument("--skip", type=float, default=0.0,
                    help="fraction of beats to blank (skipped beats)")
    ap.add_argument("--wsweep", action="store_true",
                    help="sweep the analysis window length and shape")
    ap.add_argument("--dump", action="store_true",
                    help="dump the in-band spectra instead of a table")
    ap.add_argument("--all", action="store_true",
                    help="run every estimator")
    args = ap.parse_args()

    bpm_min, bpm_max = (float(v) for v in args.band.split("-"))
    band = (bpm_min, bpm_max)
    bpms = [int(v) for v in args.bpms.split(",")]
    styles = [args.style] if args.style else STYLES

    if args.wsweep:
        print("%-6s %-6s %6s %8s %8s %s" % ("win", "kind", "wrong", "maxerr", "rmserr", "worst"))
        for kind in ("hann", "rect"):
            for win in (6.0, 7.0, 8.0, 9.0, 10.0, 12.0):
                errs = []
                wrong = 0
                worst = ""
                for style in STYLES:
                    for bpm in (90, 100, 120, 128, 140, 150, 160, 174):
                        x = make_track(bpm, style=style, seed=_seed(style, bpm),
                                       human=args.human, noise=args.noise)
                        e, sc = estimate(x, "R2_3_2", band, win, kind=kind)
                        if e is None:
                            wrong += 1
                            continue
                        err = e - bpm
                        errs.append(err)
                        if abs(err) >= 2.0:
                            wrong += 1
                            worst = "%s %d -> %.1f" % (style, bpm, e)
                errs = np.array(errs)
                print("%-6.1f %-6s %6d %8.2f %8.2f %s" % (
                    win, kind, wrong, np.max(np.abs(errs)),
                    np.sqrt(np.mean(errs ** 2)), worst))
        return 0

    if args.dump:
        for style in styles:
            for bpm in bpms:
                x = make_track(bpm, style=style, seed=_seed(style, bpm),
                               human=args.human, noise=args.noise)
                odfs = make_odfs(x)
                print("=== %s %d BPM" % (style, bpm))
                dump(odfs, band, args.window, RATE, bpm)
        return 0

    ests = ESTIMATORS if args.all else (args.est,)
    for est in ests:
        print("=" * 68)
        print("estimator %s   band %g-%g   window %.1f s   human %.1f ms   noise %.3f" % (
            est, bpm_min, bpm_max, args.window, args.human, args.noise))
        print("%-10s %6s %10s %8s %10s %s" % ("style", "bpm", "estimated", "error", "score", "verdict"))
        fails = 0
        total = 0
        per_style = {}
        for style in styles:
            c = 0
            for bpm in bpms:
                x = make_track(bpm, style=style, seed=_seed(style, bpm),
                               human=args.human, noise=args.noise)
                est_bpm, score = estimate(x, est, band, args.window,
                                         args.skip or None)
                total += 1
                if est_bpm is None:
                    print("%-10s %6d %10s %8s %10.2f NO ESTIMATE" % (style, bpm, "-", "-", score))
                    c += 1
                    fails += 1
                    continue
                err = est_bpm - bpm
                ok = abs(err) < 2.0
                if not ok:
                    c += 1
                    fails += 1
                print("%-10s %6d %10.2f %8.2f %10.2f %s" % (
                    style, bpm, est_bpm, err, score, "ok" if ok else "WRONG"))
            per_style[style] = c
        print()
        print("%d wrong of %d   %s" % (
            fails, total, " ".join("%s:%d" % (s[:4], c) for s, c in per_style.items())))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())

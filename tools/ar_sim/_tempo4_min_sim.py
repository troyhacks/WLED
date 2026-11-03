#!/usr/bin/env python3
"""The MINIMAL tempo estimator, measured.

The angle for this design is minimal-but-correct: an ODF computed from what
the AR ALREADY MEASURES (the 31 third-octave band powers), an autocorrelation
over a documented window, and a small PLL for the beat phase.

The digest says the classical ACF peak-picking loses to a spectral peak with
a 3:2 tie-break (10-11 of 48 wrong against 0-1).  That measurement was made
on the FULL ODF, where the 16th-note hat grid puts non-octave rational peaks
as tall as the beat.  This file asks the question that decides the design:

    does an ACF over the KICK-BAND ODF, with the 3:2 tie-break applied to the
    ACF's own peak family, match the spectral route?

Two ODFs are compared:
  * kick_ideal - a brick 40-180 Hz band, per-frame mean square, dB, rectified
    difference (what the digest measured).
  * ar_bands  - what the firmware can get for FREE: the AR's own 31
    third-octave band powers (estRtaPowDb), per-frame dB difference, summed
    over the kick bands.  This is the ODF the design intends to ship.

Run:  python tools/ar_sim/_tempo4_min_sim.py
"""

import argparse
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _tempo2_sim import STYLES, _seed, make_track   # noqa: E402

SR = 48000
HOP = 512
RATE = SR / HOP                    # 93.75 frames per second

# The AR's own third-octave centres (audio_reactive.h layoutISOThirdOctave).
ISO = [10 ** ((13 + k) * 0.1) for k in range(31)]
EDGE_LO = [c / 2 ** (1 / 6.0) for c in ISO]
EDGE_HI = [c * 2 ** (1 / 6.0) for c in ISO]

# The kick set the AR already has: the bottom eight bands (19.95-100 Hz) and
# the top bank's first band (125.89 Hz).  AR_BANK_BANDS = 8, AR_RTA_TOP_FIRST = 8.
KICK_BANDS = list(range(9))

WINDOW_S = 12.0
LEN = int(WINDOW_S * RATE)        # 1125 frames
EPS = 1e-12


# --------------------------------------------------------------------------
# The ODFs
# --------------------------------------------------------------------------

def odf_kick_ideal(x, lo=40.0, hi=180.0):
    """A brick kick band, per-frame mean square in dB, rectified difference."""
    n = len(x)
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(n, 1.0 / SR)
    m = ((f >= lo) & (f < hi)).astype(np.float64)
    y = np.fft.irfft(X * m, n=n)
    n2 = (len(y) // HOP) * HOP
    fr = y[:n2].reshape(-1, HOP)
    p = (fr.astype(np.float64) ** 2).mean(axis=1)
    db = 10.0 * np.log10(p + EPS)
    return np.maximum(np.diff(db, prepend=db[0]), 0.0)


def odf_ar_bands(x, bands=None, agg="sum"):
    """THE FREE ODF: the AR's own third-octave band powers.

    Each band is the integrated power over that band; the AR's estRtaPowDb is
    10*log10 of it, once per frame.  The ODF is the half-wave rectified dB
    difference, aggregated over the kick bands.
    """
    bands = KICK_BANDS if bands is None else bands
    n = len(x)
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(n, 1.0 / SR)
    flux = None
    for k in bands:
        m = ((f >= EDGE_LO[k]) & (f < EDGE_HI[k])).astype(np.float64)
        y = np.fft.irfft(X * m, n=n)
        n2 = (len(y) // HOP) * HOP
        fr = y[:n2].reshape(-1, HOP)
        p = (fr.astype(np.float64) ** 2).mean(axis=1)
        db = 10.0 * np.log10(p + EPS)
        fl = np.maximum(np.diff(db, prepend=db[0]), 0.0)
        flux = fl if flux is None else (flux + fl if agg == "sum"
                                        else np.maximum(flux, fl))
    return flux


# --------------------------------------------------------------------------
# The estimators
# --------------------------------------------------------------------------

def _parabolic(y, k):
    """Sub-bin offset of the peak at index k."""
    if k <= 0 or k >= len(y) - 1:
        return 0.0
    a, b, c = y[k - 1], y[k], y[k + 1]
    den = a - 2 * b + c
    if abs(den) < 1e-20:
        return 0.0
    d = 0.5 * (a - c) / den
    return d if abs(d) < 1.0 else 0.0


def _local_peaks(v, lo, hi):
    """(index, value) of the local maxima of v inside [lo, hi]."""
    out = []
    for k in range(max(1, lo), min(len(v) - 1, hi + 1)):
        if v[k] >= v[k - 1] and v[k] > v[k + 1]:
            out.append((k, v[k]))
    return out


def _tiebreak_32(best_bpm, best_mag, peaks, tol_db=3.0, max_ratio=4):
    """The 3:2 rule: a LOWER peak within tol_db whose BPM ratio to the winner
    is a simple rational p/q (p,q <= max_ratio) wins instead."""
    limit = best_mag / (10 ** (tol_db / 20.0))
    for bpm, mag in peaks:
        if bpm >= best_bpm:
            continue
        if mag <= 0 or mag < limit:
            continue
        r = best_bpm / bpm
        for q in range(1, max_ratio + 1):
            for p in range(q + 1, max_ratio + 1):
                if abs(r - p / q) < 0.01 * (p / q):
                    return bpm, mag
    return best_bpm, best_mag


def acf_estimate(odf, bpm_lo, bpm_hi, tie32=True, edge_guard=False,
                 norm="overlap"):
    """The ACF route: sliding-window autocorrelation, peak, parabolic, 3:2.

    norm="overlap" divides r[L] by the number of OVERLAPPING samples (N-L).
    This matters more than it looks: the raw sum r[L] falls off with lag
    because fewer pairs overlap, so the raw argmax always prefers the
    SHORTEST lag - the finest grid, not the beat.  Dividing by (N-L) makes
    the peaks comparable, which is the difference between an ACF that finds
    the beat and one that finds the sixteenth note.
    """
    if len(odf) < LEN:
        return None, 0.0, 0.0
    x = odf[-LEN:].astype(np.float64)
    x = x - x.mean()
    r = np.correlate(x, x, mode="full")[len(x) - 1:]
    n = len(x)
    # lag L frames -> BPM = 60*R/L
    lag_lo = max(1, int(math.floor(60.0 * RATE / bpm_hi)))
    lag_hi = min(n - 2, int(math.ceil(60.0 * RATE / bpm_lo)))
    if lag_hi <= lag_lo:
        return None, 0.0, 0.0
    if norm == "overlap":
        w = r[lag_lo:lag_hi + 1] / np.arange(n - lag_lo, n - lag_hi - 1, -1)
    else:
        w = r[lag_lo:lag_hi + 1] / max(r[0], 1e-20)
    k = int(np.argmax(w))
    lag = lag_lo + k
    bpm = 60.0 * RATE / lag
    mag = float(w[k])
    # parabolic refinement in LAG space -> sub-lag period
    d = _parabolic(w, k)
    if d != 0.0:
        lag_f = lag + d
        if lag_f > 1.0:
            bpm = 60.0 * RATE / lag_f
    peaks = []
    for i, m in _local_peaks(w, 0, len(w) - 1):
        L = lag_lo + i
        if L <= 0:
            continue
        peaks.append((60.0 * RATE / L, float(m)))
    if tie32 and peaks:
        bpm, mag = _tiebreak_32(bpm, mag, peaks)
    edge = 0.0
    if edge_guard:
        # The winner at a band edge is a clipped truth, not a tempo.
        if k == 0 or k == len(w) - 1:
            edge = 1.0
    return bpm, mag, edge


def spec_estimate(odf, bpm_lo, bpm_hi, tie32=True, edge_guard=False):
    """The spectral route: zero-padded DFT of the window, tallest in-band
    peak, parabolic refinement on the log magnitude, 3:2 tie-break."""
    if len(odf) < LEN:
        return None, 0.0, 0.0
    x = odf[-LEN:].astype(np.float64)
    x = x - x.mean()
    nfft = 4096
    X = np.fft.rfft(x, nfft)
    P = np.abs(X)
    fr = np.fft.rfftfreq(nfft, 1.0 / RATE)
    sel = np.where((fr * 60.0 >= bpm_lo) & (fr * 60.0 <= bpm_hi))[0]
    if len(sel) < 3:
        return None, 0.0, 0.0
    bpm = fr[sel] * 60.0
    mag = P[sel]
    k = int(np.argmax(mag))
    best_bpm, best_mag = float(bpm[k]), float(mag[k])
    # parabolic on the log magnitude
    if 0 < k < len(sel) - 1:
        lm = np.log(mag + 1e-20)
        d = _parabolic(lm, k)
        if d != 0.0:
            step = 60.0 / nfft * RATE
            best_bpm += d * step
    peaks = []
    for i, m in _local_peaks(mag, 0, len(mag) - 1):
        peaks.append((float(bpm[i]), float(m)))
    if tie32 and peaks:
        best_bpm, best_mag = _tiebreak_32(best_bpm, best_mag, peaks)
    edge = 1.0 if (k == 0 or k == len(sel) - 1) else 0.0
    return best_bpm, best_mag, edge


# --------------------------------------------------------------------------
# The phase detector: the complex DFT at the beat frequency.
# --------------------------------------------------------------------------

def phase_estimate(odf, bpm, win_frames=int(RATE)):
    """The phase of the beat, from the complex DFT at the beat frequency over
    a one-second window.  Returns (phase in [0,1) of a beat, strength)."""
    if len(odf) < win_frames or bpm <= 0:
        return None, 0.0
    x = odf[-win_frames:].astype(np.float64)
    x = x - x.mean()
    f = bpm / 60.0
    n = np.arange(len(x))
    ang = 2.0 * math.pi * f * n / RATE
    re = float(np.dot(x, np.cos(ang)))
    im = float(np.dot(x, np.sin(ang)))
    mag = math.hypot(re, im)
    if mag <= 1e-20:
        return None, 0.0
    ph = math.atan2(im, re) / (2.0 * math.pi)   # in cycles
    ph -= math.floor(ph)
    return ph, mag


def truth_kick_times(bpm, seconds=16.0):
    """The kick times the generator used, for the phase error."""
    beat = 60.0 / bpm
    return [b * beat for b in range(int(seconds / beat))]


def phase_error_ms(odf, bpm, style, seconds=16.0):
    """How far the published phase is from a real beat, in ms.

    The published phase is a fraction of a beat; the beat times are known.
    The error is the distance from the phase to the nearest beat."""
    ph, strength = phase_estimate(odf, bpm)
    if ph is None:
        return None
    period = 60.0 / bpm
    # The DFT phase is referenced to the END of the window.  The window
    # covers [t_end - win, t_end]; the phase says where the beat grid sits
    # inside it.
    win = int(RATE)
    t_end = len(odf) / RATE
    # beat grid times: t = t_end - win + ph*period + n*period
    t0 = t_end - win / RATE + ph * period
    # find the beat grid line closest to a truth kick
    best = None
    for kt in truth_kick_times(bpm, seconds):
        # distance from kt to the grid
        n = round((kt - t0) / period)
        d = abs(t0 + n * period - kt) * 1000.0
        if best is None or d < best:
            best = d
    return best


# --------------------------------------------------------------------------
# The sweep
# --------------------------------------------------------------------------

_CACHE = {}


def cached_odfs(style, bpm, human, noise):
    """The ODFs for one track, computed ONCE and reused by every sweep."""
    key = (style, bpm, human, noise)
    if key not in _CACHE:
        x = make_track(bpm, style=style, seed=_seed(style, bpm),
                       human=human, noise=noise)
        _CACHE[key] = {
            "kick_ideal": odf_kick_ideal(x),
            "ar_bands": odf_ar_bands(x),
        }
    return _CACHE[key]


def run(band, odf_name, estimator, tie32, edge_guard, human, noise, skip, bpms):
    return run2(band, odf_name, estimator, tie32, "overlap",
                human, noise, skip, bpms)


def run2(band, odf_name, estimator, tie32, norm, human, noise, skip, bpms):
    bpm_lo, bpm_hi = band
    errs = []
    wrong = 0
    total = 0
    edge_reported = 0
    rows = []
    for style in STYLES:
        for bpm in bpms:
            odf = cached_odfs(style, bpm, human, noise)[odf_name]
            if skip:
                rng = np.random.default_rng(int(bpm) + 7)
                kill = rng.random(len(odf)) < skip * 0.35
                odf = odf.copy()
                odf[kill] = 0.0
            if estimator == "acf":
                e, mag, edge = acf_estimate(odf, bpm_lo, bpm_hi, tie32,
                                            False, norm)
            else:
                e, mag, edge = spec_estimate(odf, bpm_lo, bpm_hi, tie32,
                                             False)
            total += 1
            if e is None:
                wrong += 1
                rows.append((style, bpm, None, None, 0.0))
                continue
            err = e - bpm
            errs.append(err)
            if abs(err) >= 2.0:
                wrong += 1
            if edge:
                edge_reported += 1
            rows.append((style, bpm, e, err, edge))
    errs = np.array(errs) if errs else np.array([0.0])
    return wrong, total, errs, edge_reported, rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bpms", default="90,100,110,120,128,140,150,160,174")
    ap.add_argument("--human", type=float, default=0.0)
    ap.add_argument("--noise", type=float, default=0.0)
    ap.add_argument("--skip", type=float, default=0.0)
    ap.add_argument("--detail", action="store_true")
    args = ap.parse_args()
    bpms = [int(v) for v in args.bpms.split(",")]

    bands = [(120.0, 176.0), (90.0, 175.0), (90.0, 150.0)]
    print("=== estimator comparison, %d styles x %d tempi, window %.1f s ===" %
          (len(STYLES), len(bpms), WINDOW_S))
    print("%-6s %-11s %-11s %-5s %6s %9s %8s  %s" %
          ("band", "odf", "estimator", "t32", "wrong", "maxerr", "rmserr", "worst"))
    for band in bands:
        for odf_name in ("kick_ideal", "ar_bands"):
            for est, norm in (("acf", "overlap"), ("acf", "r0"),
                              ("spec", "-")):
                for tie32 in (False, True):
                    wrong, total, errs, edge_rep, rows = run2(
                        band, odf_name, est, tie32, norm,
                        args.human, args.noise, args.skip, bpms)
                    worst = ""
                    if wrong:
                        for style, bpm, e, err, edge in rows:
                            if e is None or abs(err) >= 2.0:
                                worst = "%s %d -> %s" % (
                                    style, bpm, "none" if e is None else "%.1f" % e)
                                break
                    name = est if est == "spec" else "acf_" + norm
                    print("%-6s %-11s %-11s %-5s %6d %9.2f %8.2f  %s" % (
                        "%g-%g" % band, odf_name, name, tie32, wrong,
                        np.max(np.abs(errs)), np.sqrt(np.mean(errs ** 2)), worst))
    print()

    # The edge guard: how many of the band-clipped truths are at a band edge.
    print("=== band-edge guard (winner at the band edge = a clipped truth) ===")
    for band in bands:
        for odf_name in ("kick_ideal", "ar_bands"):
            wrong, total, errs, edge_rep, rows = run(
                band, odf_name, "acf", True, True,
                args.human, args.noise, args.skip, bpms)
            # how many WRONG answers were flagged at an edge
            wrong_edge = sum(1 for style, bpm, e, err, edge in rows
                             if e is not None and abs(err) >= 2.0 and edge)
            print("%-6s %-11s wrong %2d, of which at a band edge %2d" %
                  ("%g-%g" % band, odf_name, wrong, wrong_edge))
    print()

    # The phase detector's error against the known kick times.
    print("=== phase error against the generator's own kick times ===")
    for bpm in (120.0, 128.0, 140.0, 174.0):
        errs = []
        for style in STYLES:
            x = make_track(bpm, style=style, seed=_seed(style, bpm))
            odf = odf_kick_ideal(x)
            e = phase_error_ms(odf, bpm, style)
            if e is not None:
                errs.append(e)
        print("  %6.1f BPM  phase error: mean %.1f ms, worst %.1f ms" %
              (bpm, np.mean(errs), np.max(errs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

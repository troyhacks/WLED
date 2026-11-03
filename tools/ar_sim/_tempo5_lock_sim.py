#!/usr/bin/env python3
"""The questions the tempo design depends on, MEASURED.

This file answers, with numbers:

Q1  Which of the AR's own third-octave bands make the best ODF?  Sum or max?
    Does the 3:2 tie-break help or hurt on the FREE ODF, and at what
    tolerance?
Q1b How fine must the DFT grid be (N=1024/2048/4096)?
Q1c Is a running DFT with a RECTANGULAR window (what the firmware would
    implement) as good as a complex RESONATOR with an exponential window
    (no twiddle table)?  The resonator is much cheaper but its spectrum has
    1/f^2 tails, so DC and low-frequency leakage is worse.
Q2  WHY does the autocorrelation lose to the spectral peak?  (the short-lag
    bias, and the 3:2 rival the ACF's lattice cannot represent)
Q3  Which confidence number separates the right answers from the wrong ones?
Q4  How long does the estimate take to lock?
Q5  Robustness: timing jitter, blanked beats, broadband noise.
Q6  The phase detector's beat-time error, with the window-centre reference.

The estimator is the firmware's: a running DFT over a 12 s window on a
4096-point grid, whose magnitudes are IDENTICAL to the zero-padded FFT of the
window (the running DFT only differs by a phase rotation per bin), so the
measurement can use numpy's FFT while the firmware uses the running form.

Run:  python tools/ar_sim/_tempo5_lock_sim.py [q1|q2|q3|q4|q5|q6|all]
"""

import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _tempo2_sim import STYLES, _seed, make_track   # noqa: E402

SR = 48000
HOP = 512
RATE = SR / HOP                    # 93.75 frames per second

# The AR's own third-octave centres and edges (audio_reactive.h
# layoutISOThirdOctave): 19.95 Hz .. 19.95 kHz, one third of an octave wide.
ISO = [10 ** ((13 + k) * 0.1) for k in range(31)]
EDGE_LO = [c / 2 ** (1 / 6.0) for c in ISO]
EDGE_HI = [c * 2 ** (1 / 6.0) for c in ISO]

WINDOW_S = 12.0
LEN = int(WINDOW_S * RATE)        # 1125 frames
EPS = 1e-12

# The band sets the AR can supply for free: the bottom bank holds bands 0-7
# (19.95-100 Hz) at 3 kHz, the top bank holds band 8 (125.89 Hz) upwards at
# 48 kHz.  A kick lives at 45-70 Hz with a click that reaches ~150 Hz.
NBAND = 10

# The tempo band candidates.
BAND_CLUB = (120.0, 176.0)
BAND_WIDE = (90.0, 175.0)
BAND_TROY = (90.0, 150.0)

TRUTHS = [90, 100, 110, 120, 128, 140, 150, 160, 174]


# --------------------------------------------------------------------------
# The ODF from the AR's own band powers
# --------------------------------------------------------------------------

_TRACK_CACHE = {}


def track_bands(style, bpm, human=0.0, noise=0.0, nb=NBAND):
    """The per-frame dB of the AR's first nb third-octave bands.

    Cached: the whole Q1 sweep re-uses these.
    """
    key = (style, bpm, human, noise, nb)
    if key in _TRACK_CACHE:
        return _TRACK_CACHE[key]
    x = make_track(bpm, style=style, seed=_seed(style, bpm),
                   human=human, noise=noise)
    n = len(x)
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(n, 1.0 / SR)
    out = []
    for k in range(nb):
        m = ((f >= EDGE_LO[k]) & (f < EDGE_HI[k])).astype(np.float64)
        y = np.fft.irfft(X * m, n=n)
        n2 = (len(y) // HOP) * HOP
        fr = y[:n2].reshape(-1, HOP)
        p = (fr.astype(np.float64) ** 2).mean(axis=1)
        out.append(10.0 * np.log10(p + EPS))
    _TRACK_CACHE[key] = out
    return out


def odf_of(bands, style, bpm, human=0.0, noise=0.0, agg="sum"):
    """The free ODF: the rectified frame-to-frame dB difference, summed (or
    maxed) over the chosen third-octave bands."""
    db = track_bands(style, bpm, human, noise)
    flux = None
    for k in bands:
        fl = np.maximum(np.diff(db[k], prepend=db[k][0]), 0.0)
        flux = fl if flux is None else (flux + fl if agg == "sum"
                                        else np.maximum(flux, fl))
    return flux


# --------------------------------------------------------------------------
# The estimators
# --------------------------------------------------------------------------

def _parabolic(y, k):
    if k <= 0 or k >= len(y) - 1:
        return 0.0
    a, b, c = y[k - 1], y[k], y[k + 1]
    den = a - 2 * b + c
    if abs(den) < 1e-20:
        return 0.0
    d = 0.5 * (a - c) / den
    return d if abs(d) < 1.0 else 0.0


def bin_range(bpm_lo, bpm_hi, nfft, rate=RATE):
    k_min = int(math.floor(bpm_lo / 60.0 * nfft / rate)) - 1
    k_max = int(math.ceil(bpm_hi / 60.0 * nfft / rate)) + 1
    return k_min, k_max


def window_spectrum(odf, nfft=4096):
    """The window's spectrum.  For the firmware's running DFT the magnitudes
    are IDENTICAL to this; only the phase reference differs."""
    x = odf[-LEN:].astype(np.float64)
    x = x - x.mean()
    return np.fft.rfft(x, nfft)


def resonator(odf, bpm_lo, bpm_hi, tau_s, nfft=4096):
    """A bank of complex resonators: A_k[n] = rho*exp(-j2*pi*f_k/RATE)*A[n-1]
    + x[n].  No twiddle table, one complex multiply per bin per frame, but
    the effective window is EXPONENTIAL, so the spectrum has 1/f^2 tails.

    Returns the fair magnitude: |A| * |1 - rho*exp(-j2*pi*f_k/RATE)|, which
    removes the resonator's own gain so the candidates are comparable.
    """
    x = odf.astype(np.float64)
    x = x - x.mean()
    k_min, k_max = bin_range(bpm_lo, bpm_hi, nfft)
    ks = np.arange(k_min, k_max + 1)
    f = ks * RATE / nfft
    rho = math.exp(-1.0 / (tau_s * RATE))
    pole = rho * np.exp(-2j * math.pi * f / RATE)
    A = np.zeros(len(ks), dtype=complex)
    for i in range(len(x)):
        A = A * pole + x[i]
    fair = np.abs(A) * np.abs(1.0 - pole)
    return ks, fair


def acf_peaks(odf, bpm_lo, bpm_hi):
    """The overlap-normalised autocorrelation's local peaks, as (bpm, mag)."""
    x = odf[-LEN:].astype(np.float64)
    x = x - x.mean()
    n = len(x)
    r = np.correlate(x, x, mode="full")[n - 1:]
    lag_lo = max(1, int(math.floor(60.0 * RATE / bpm_hi)))
    lag_hi = min(n - 2, int(math.ceil(60.0 * RATE / bpm_lo)))
    w = r[lag_lo:lag_hi + 1] / np.arange(n - lag_lo, n - lag_hi - 1, -1)
    peaks = []
    for i in range(1, len(w) - 1):
        if w[i] >= w[i - 1] and w[i] > w[i + 1]:
            peaks.append((60.0 * RATE / (lag_lo + i), float(w[i])))
    if not peaks:
        return None, None, peaks
    k = int(np.argmax(w))
    return 60.0 * RATE / (lag_lo + k), float(w[k]), peaks


def spec_peaks(ks, mags):
    """The spectrum's local peaks, as (bpm, mag)."""
    bpms = ks * RATE / 4096.0 * 60.0
    peaks = []
    for i in range(1, len(mags) - 1):
        if mags[i] >= mags[i - 1] and mags[i] > mags[i + 1]:
            peaks.append((float(bpms[i]), float(mags[i])))
    return peaks


def tie_break(best_bpm, best_mag, peaks, tol_db, ratios):
    """The rational tie-break: a LOWER peak within tol_db whose BPM ratio to
    the winner is one of `ratios` (as p/q with p>q) wins instead.

    ratios: a list of (p, q) pairs, e.g. [(3,2)] for 3:2 only.
    """
    if not peaks:
        return best_bpm, best_mag, 0.0
    limit = best_mag / (10 ** (tol_db / 20.0))
    for bpm, mag in peaks:
        if bpm >= best_bpm or mag < limit or mag <= 0:
            continue
        r = best_bpm / bpm
        for (p, q) in ratios:
            if abs(r - p / q) < 0.012 * (p / q):
                return bpm, mag, r
    return best_bpm, best_mag, 0.0


def estimate_spectral(odf, band, tol_db=3.0, ratios=((3, 2),), nfft=4096,
                      subtract_mean=True):
    """The firmware's estimator: the running DFT's tallest in-band peak,
    parabolic-refined, with the rational tie-break."""
    bpm_lo, bpm_hi = band
    k_min, k_max = bin_range(bpm_lo, bpm_hi, nfft)
    ks = np.arange(k_min, k_max + 1)
    x = odf[-LEN:].astype(np.float64)
    if subtract_mean:
        x = x - x.mean()
    X = np.fft.rfft(x, nfft)
    mags = np.abs(X[k_min:k_max + 1])
    bpms = ks * RATE / nfft * 60.0
    keep = (bpms >= bpm_lo) & (bpms <= bpm_hi)
    idx = np.where(keep)[0]
    if len(idx) < 3:
        return None, 0.0, 0.0
    sub = mags[idx]
    sb = bpms[idx]
    k = int(np.argmax(sub))
    best = float(sb[k])
    mag = float(sub[k])
    d = _parabolic(np.log(sub + 1e-20), k)
    if d:
        best += d * (RATE / nfft * 60.0)
    peaks = []
    for i in range(1, len(sub) - 1):
        if sub[i] >= sub[i - 1] and sub[i] > sub[i + 1]:
            peaks.append((float(sb[i]), float(sub[i])))
    best, mag, ratio = tie_break(best, mag, peaks, tol_db, ratios)
    # the runner-up for the confidence: the tallest peak that is NOT a
    # rational partner of the winner
    conf_dB = 0.0
    runner = 0.0
    for bpm, m in peaks:
        if abs(bpm - best) < 0.5:
            continue
        r = max(best / bpm, bpm / best)
        rat = None
        for (p, q) in ((2, 1), (3, 1), (4, 1), (3, 2), (4, 3)):
            if abs(r - p / q) < 0.012 * (p / q):
                rat = (p, q)
                break
        if rat is None and m > runner:
            runner = m
    if runner > 0 and mag > 0:
        conf_dB = 20.0 * math.log10(mag / runner)
    return best, mag, conf_dB


def estimate_acf(odf, band, tol_db=3.0, ratios=((3, 2),)):
    """The classic route, for the comparison."""
    bpm_lo, bpm_hi = band
    best, mag, peaks = acf_peaks(odf, bpm_lo, bpm_hi)
    if best is None:
        return None, 0.0, 0.0
    best, mag, _ = tie_break(best, mag, peaks, tol_db, ratios)
    return best, mag, 0.0


# --------------------------------------------------------------------------
# Q1: the band set, the aggregation, the tie-break's tolerance
# --------------------------------------------------------------------------

SETS = [
    ("0-8", range(0, 9)),
    ("1-8", range(1, 9)),
    ("2-8", range(2, 9)),
    ("3-8", range(3, 9)),
    ("4-8", range(4, 9)),
    ("5-8", range(5, 9)),
    ("3-9", range(3, 10)),
    ("4-9", range(4, 10)),
    ("2-9", range(2, 10)),
]


def sweep(label, combos, band, truths=TRUTHS, human=0.0, noise=0.0):
    rows = []
    for name, bands, agg, tol, ratios in combos:
        errs = []
        wrong = 0
        worst = ""
        for style in STYLES:
            for bpm in truths:
                odf = odf_of(list(bands), style, bpm, human, noise, agg)
                e, mag, _ = estimate_spectral(odf, band, tol, ratios)
                if e is None:
                    wrong += 1
                    worst = "%s %d -> none" % (style, bpm)
                    continue
                err = e - bpm
                errs.append(err)
                if abs(err) >= 2.0:
                    wrong += 1
                    worst = "%s %d -> %.1f" % (style, bpm, e)
        errs = np.array(errs) if errs else np.array([0.0])
        rows.append((name, wrong, np.max(np.abs(errs)),
                     np.sqrt(np.mean(errs ** 2)), worst))
    print("%-14s %6s %9s %9s  %s" % (label, "wrong", "maxerr", "rmserr", "worst"))
    for r in rows:
        print("%-14s %6d %9.2f %9.2f  %s" % r)
    print()
    return rows


def q1():
    print("=== Q1a: the ODF's band set and aggregation (band %g-%g) ===" % BAND_WIDE)
    combos = []
    for name, bands in SETS:
        for agg in ("sum", "max"):
            combos.append(("%s/%s" % (name, agg), list(bands), agg, 3.0, ((3, 2),)))
    sweep("band/agg", combos, BAND_WIDE)
    return


def q1b():
    print("=== Q1b: the tie-break's tolerance and ratio set (band %g-%g, 3-8/sum) ==="
          % BAND_WIDE)
    combos = []
    for tol in (0.0, 1.0, 2.0, 3.0, 4.0, 6.0):
        combos.append(("tol %.0f dB 3:2" % tol, list(range(3, 9)), "sum", tol, ((3, 2),)))
    combos.append(("off", list(range(3, 9)), "sum", 3.0, ()))
    combos.append(("3dB 3:2+2:1", list(range(3, 9)), "sum", 3.0, ((3, 2), (2, 1))))
    combos.append(("3dB all p/q", list(range(3, 9)), "sum", 3.0,
                   ((2, 1), (3, 1), (4, 1), (3, 2), (4, 3))))
    sweep("tie-break", combos, BAND_WIDE)
    return


def q1c():
    print("=== Q1c: the DFT grid size and the resonator (band %g-%g, 3-8/sum) ==="
          % BAND_WIDE)
    print("--- grid size (running DFT, rectangular window) ---")
    for nfft in (1024, 2048, 4096, 8192):
        errs = []
        wrong = 0
        worst = ""
        for style in STYLES:
            for bpm in TRUTHS:
                odf = odf_of(list(range(3, 9)), style, bpm)
                e, mag, _ = estimate_spectral(odf, BAND_WIDE, 3.0, ((3, 2),),
                                              nfft=nfft)
                if e is None:
                    wrong += 1
                    continue
                err = e - bpm
                errs.append(err)
                if abs(err) >= 2.0:
                    wrong += 1
                    worst = "%s %d -> %.1f" % (style, bpm, e)
        errs = np.array(errs)
        print("N=%5d  wrong %2d  maxerr %7.2f  rms %6.2f  %s" %
              (nfft, wrong, np.max(np.abs(errs)),
               np.sqrt(np.mean(errs ** 2)), worst))
    print()
    print("--- the resonator (exponential window, no twiddle table) ---")
    for tau in (3.0, 6.0, 12.0, 20.0):
        errs = []
        wrong = 0
        worst = ""
        for style in STYLES:
            for bpm in TRUTHS:
                odf = odf_of(list(range(3, 9)), style, bpm)
                ks, mags = resonator(odf, BAND_WIDE[0], BAND_WIDE[1], tau)
                bpms = ks * RATE / 4096.0 * 60.0
                keep = (bpms >= BAND_WIDE[0]) & (bpms <= BAND_WIDE[1])
                sub = mags[keep]
                sb = bpms[keep]
                k = int(np.argmax(sub))
                e = float(sb[k])
                d = _parabolic(np.log(sub + 1e-20), k)
                if d:
                    e += d * (RATE / 4096.0 * 60.0)
                err = e - bpm
                errs.append(err)
                if abs(err) >= 2.0:
                    wrong += 1
                    worst = "%s %d -> %.1f" % (style, bpm, e)
        errs = np.array(errs)
        print("tau=%5.1f s  wrong %2d  maxerr %7.2f  rms %6.2f  %s" %
              (tau, wrong, np.max(np.abs(errs)),
               np.sqrt(np.mean(errs ** 2)), worst))
    print()
    return


def q2():
    print("=== Q2: why the ACF loses ===")
    print("The ACF's overlap-normalised peak height falls as 1/L for a sparse")
    print("onset train, so its argmax drifts to the SHORTEST lag in range.")
    print()
    print("%-9s %6s %9s %9s %9s %9s %9s" %
          ("style", "bpm", "acf_bpm", "acf_lag", "true_lag", "dft_bpm", "dft_bpm/acf"))
    ratios = []
    for style in STYLES:
        for bpm in (128, 140, 150):
            odf = odf_of(list(range(3, 9)), style, bpm)
            a, am, peaks = acf_peaks(odf, BAND_WIDE[0], BAND_WIDE[1])
            d, dm, _ = estimate_spectral(odf, BAND_WIDE, 3.0, ((3, 2),))
            a_lag = 60.0 * RATE / a
            t_lag = 60.0 * RATE / bpm
            ratios.append(a / bpm)
            print("%-9s %6d %9.1f %9.2f %9.2f %9.1f %9.3f" %
                  (style, bpm, a, a_lag, t_lag, d, a / bpm))
    print()
    print("the ACF's mean bpm/truth ratio: %.3f (1.0 = right, 1.5 = the dotted"
          " reading, 2.0 = the half reading)" % np.mean(ratios))
    print()
    print("--- is the 3:2 rival visible to each estimator? ---")
    print("%-9s %6s %11s %11s %11s %11s" %
          ("style", "bpm", "dft_win", "dft_3:2", "acf_win", "acf_3:2"))
    for style in STYLES:
        for bpm in (128, 140, 150):
            odf = odf_of(list(range(3, 9)), style, bpm)
            ks, mags = None, None
            x = odf[-LEN:].astype(np.float64)
            x = x - x.mean()
            X = np.abs(np.fft.rfft(x, 4096))
            fr = np.fft.rfftfreq(4096, 1.0 / RATE) * 60.0
            kf = int(round((bpm / 60.0) * 4096 / RATE))
            kr = int(round(kf * 1.5))
            dft_win = float(X[kf])
            dft_riv = float(X[kr]) if kr < len(X) else 0.0
            a, am, peaks = acf_peaks(odf, BAND_WIDE[0], BAND_WIDE[1])
            al = 60.0 * RATE / a
            r3 = int(round(al * 1.5))
            xr = odf[-LEN:].astype(np.float64)
            xr = xr - xr.mean()
            r = np.correlate(xr, xr, mode="full")[len(xr) - 1:]
            w = r / np.arange(len(r), 0, -1)
            acf_riv = float(w[r3]) if r3 < len(w) else 0.0
            print("%-9s %6d %11.4f %11.4f %11.1f %11.4f" %
                  (style, bpm, dft_win, dft_riv, a, acf_riv))
    print()
    return


def q3():
    print("=== Q3: the confidence that separates right from wrong ===")
    print("%-9s %6s %8s %9s %9s %8s" %
          ("style", "bpm", "verdict", "bpm_est", "margin_dB", "share"))
    ok_marg = []
    bad_marg = []
    ok_share = []
    bad_share = []
    for style in STYLES:
        for bpm in TRUTHS:
            odf = odf_of(list(range(3, 9)), style, bpm)
            e, mag, marg = estimate_spectral(odf, BAND_WIDE, 3.0, ((3, 2),))
            if e is None:
                bad_marg.append(0.0)
                bad_share.append(0.0)
                continue
            right = abs(e - bpm) < 2.0
            # the winner's share of the in-band energy
            x = odf[-LEN:].astype(np.float64)
            x = x - x.mean()
            X = np.abs(np.fft.rfft(x, 4096))
            fr = np.fft.rfftfreq(4096, 1.0 / RATE) * 60.0
            sel = (fr >= BAND_WIDE[0]) & (fr <= BAND_WIDE[1])
            share = float(X[sel].sum() and X[int(round(e / 60.0 * 4096 / RATE))]
                          / X[sel].sum())
            (ok_marg if right else bad_marg).append(marg)
            (ok_share if right else bad_share).append(share)
            print("%-9s %6d %8s %9.1f %9.2f %8.3f" %
                  (style, bpm, "right" if right else "WRONG", e, marg, share))
    print()
    print("margin  when right: mean %6.2f dB, min %6.2f" %
          (np.mean(ok_marg), np.min(ok_marg)))
    if bad_marg:
        print("margin  when wrong: mean %6.2f dB, max %6.2f" %
              (np.mean(bad_marg), np.max(bad_marg)))
    print("share   when right: mean %6.4f, min %6.4f" %
          (np.mean(ok_share), np.min(ok_share)))
    if bad_share:
        print("share   when wrong: mean %6.4f, max %6.4f" %
              (np.mean(bad_share), np.max(bad_share)))
    print()
    return


def q5():
    print("=== Q5: robustness (band %g-%g, 3-8/sum, 3 dB 3:2) ===" % BAND_WIDE)
    for label, kw in (("clean", {}),
                      ("jitter 15 ms", {"human": 15.0}),
                      ("jitter 30 ms", {"human": 30.0}),
                      ("noise 0.02", {"noise": 0.02}),
                      ("noise 0.05", {"noise": 0.05})):
        errs = []
        wrong = 0
        worst = ""
        for style in STYLES:
            for bpm in TRUTHS:
                odf = odf_of(list(range(3, 9)), style, bpm, **kw)
                e, mag, _ = estimate_spectral(odf, BAND_WIDE, 3.0, ((3, 2),))
                if e is None:
                    wrong += 1
                    continue
                err = e - bpm
                errs.append(err)
                if abs(err) >= 2.0:
                    wrong += 1
                    worst = "%s %d -> %.1f" % (style, bpm, e)
        errs = np.array(errs)
        print("%-12s wrong %2d of %2d  maxerr %7.2f  rms %6.2f  %s" %
              (label, wrong, len(STYLES) * len(TRUTHS),
               np.max(np.abs(errs)), np.sqrt(np.mean(errs ** 2)), worst))
    for frac in (0.15, 0.30):
        errs = []
        wrong = 0
        worst = ""
        for style in STYLES:
            for bpm in TRUTHS:
                odf = odf_of(list(range(3, 9)), style, bpm)
                rng = np.random.default_rng(bpm + int(frac * 100))
                odf = odf.copy()
                odf[rng.random(len(odf)) < frac] = 0.0
                e, mag, _ = estimate_spectral(odf, BAND_WIDE, 3.0, ((3, 2),))
                if e is None:
                    wrong += 1
                    continue
                err = e - bpm
                errs.append(err)
                if abs(err) >= 2.0:
                    wrong += 1
                    worst = "%s %d -> %.1f" % (style, bpm, e)
        errs = np.array(errs)
        print("%-12s wrong %2d of %2d  maxerr %7.2f  rms %6.2f  %s" %
              ("blank %.0f%%" % (frac * 100), wrong, len(STYLES) * len(TRUTHS),
               np.max(np.abs(errs)), np.sqrt(np.mean(errs ** 2)), worst))
    print()
    return


def q6():
    print("=== Q6: the phase detector's beat-time error ===")
    print("(the DFT's phase at the beat frequency, referenced to the window's")
    print(" centre, against the generator's own kick times)")
    for win_s in (0.5, 1.0, 2.0):
        for bpm in (120.0, 128.0, 140.0, 174.0):
            errs = []
            for style in STYLES:
                x = make_track(bpm, style=style, seed=_seed(style, bpm))
                odf = odf_of(list(range(3, 9)), style, bpm)
                win = int(win_s * RATE)
                v = odf[-win:].astype(np.float64)
                v = v - v.mean()
                f = bpm / 60.0
                n = np.arange(len(v))
                ang = 2.0 * math.pi * f * (n - (len(v) - 1) / 2.0) / RATE
                re = float(np.dot(v, np.cos(ang)))
                im = float(np.dot(v, np.sin(ang)))
                if abs(re) < 1e-20 and abs(im) < 1e-20:
                    continue
                # the phase in cycles of the beat
                ph = math.atan2(im, re) / (2.0 * math.pi)
                period = 60.0 / bpm
                # the ODF's frame index at the window's end
                t_end = len(odf) / RATE
                # a beat is where the ODF's complex sum peaks: the phase says
                # where in the beat cycle the grid sits, referenced to the
                # window's centre frame
                t_centre = t_end - win / (2.0 * RATE)
                t0 = t_centre - ph * period
                beat = 60.0 / bpm
                kt = [k * beat for k in range(int(16.0 / beat))]
                best = None
                for k in kt:
                    n = round((k - t0) / period)
                    d = abs(t0 + n * period - k) * 1000.0
                    if best is None or d < best:
                        best = d
                errs.append(best)
            if errs:
                print("  win %4.1f s  %6.1f BPM  mean %6.1f ms  worst %6.1f ms" %
                      (win_s, bpm, np.mean(errs), np.max(errs)))
    print()
    return


if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    if which in ("all", "q1"):
        q1()
    if which in ("all", "q1b"):
        q1b()
    if which in ("all", "q1c"):
        q1c()
    if which in ("all", "q2"):
        q2()
    if which in ("all", "q3"):
        q3()
    if which in ("all", "q5"):
        q5()
    if which in ("all", "q6"):
        q6()

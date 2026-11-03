#!/usr/bin/env python3
"""The FIRMWARE-FAITHFUL tempo model.

_tempo2_sim.py decided the ALGORITHM with an ideal brick-wall band and an
offline FFT.  That is the right way to choose an algorithm and the wrong way
to ship one: the firmware has a causal filter, a sliding window, a float32
running DFT and no FFT at all in the frame.

This file is the shadow of the C++ that will go into
usermods/audioreactive/ar_tempo.h.  Every constant here is the constant the
firmware will use, and every step is the step the firmware will take:

  * a causal band-pass cascade (transposed direct form II, float32)
  * per-frame mean square of the band-passed signal
  * 10*log10 of that, and the half-wave rectified difference between frames
  * a 12 s sliding window of those values (1125 frames at 93.75 Hz)
  * a RUNNING DFT over only the in-band bins of a 4096-point grid
    (1.373 BPM per bin) - no FFT, no bit reversal, O(1) per frame
  * the tallest in-band peak, parabolic refinement, and the R2_3_2 tie-break

The running DFT is the interesting part.  It keeps one complex accumulator per
in-band bin and slides the window:

    S_k = sum over the window of  odf[m] * exp(-j*2*pi*k*m/4096)

with m the ABSOLUTE frame index.  Adding the newest sample and removing the
oldest is one complex multiply-add each.  The phase factors come from a
4096-entry twiddle table indexed by (k*m) & 4095, so there is no accumulated
rotation and therefore no drift: each sample is added and later removed with
the SAME factor, so the error is a random walk over the window, not a gain that
grows with time.  (The textbook form, which rotates the accumulator by
exp(+j*2*pi*k/4096) every frame, has a |w| of 1 +- 6e-8, and that 6e-8 per
frame becomes 60% after a day.)

    python tools/ar_sim/_tempo3_sim.py --check          # running DFT vs numpy
    python tools/ar_sim/_tempo3_sim.py --filters        # filter shootout
    python tools/ar_sim/_tempo3_sim.py --est            # the matrix
    python tools/ar_sim/_tempo3_sim.py --est --human 3 --noise 0.02
"""

import argparse
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _tempo2_sim import STYLES, _seed, make_track, inject_skips   # noqa: E402

# --------------------------------------------------------------------------
# The constants the firmware will use
# --------------------------------------------------------------------------

SR = 48000
HOP = 512
RATE = SR / HOP              # 93.75 frames per second

WINDOW_S = 12.0
LEN = int(WINDOW_S * RATE)  # 1125 frames

NFFT = 4096                 # the zero-padded grid: 1.373 BPM per bin
BPM_LO = 90.0
BPM_HI = 175.0

# The bins the accumulator covers.  The band edges are not on a bin boundary,
# so one bin of margin on each side is kept - the parabolic refinement needs
# the neighbours of the edge bins.
K_MIN = int(math.floor(BPM_LO / 60.0 * NFFT / RATE)) - 1
K_MAX = int(math.ceil(BPM_HI / 60.0 * NFFT / RATE)) + 1
N_BINS = K_MAX - K_MIN + 1

EPS = 1e-12


# --------------------------------------------------------------------------
# The band-pass filter - the firmware's, not an ideal brick wall
# --------------------------------------------------------------------------

def rbj_lpf(f, fs, q):
    """ESP-DSP's dsps_biquad_gen_lpf_f32: b0, b1, b2, a1, a2 with a0 = 1."""
    w0 = 2.0 * math.pi * f / fs
    alpha = math.sin(w0) / (2.0 * q)
    a0 = 1.0 + alpha
    cw = math.cos(w0)
    return np.array([(1.0 - cw) / 2.0 / a0,
                     (1.0 - cw) / a0,
                     (1.0 - cw) / 2.0 / a0,
                     -2.0 * cw / a0,
                     (1.0 - alpha) / a0], dtype=np.float64)


def rbj_hpf(f, fs, q):
    """ESP-DSP's dsps_biquad_gen_hpf_f32."""
    w0 = 2.0 * math.pi * f / fs
    alpha = math.sin(w0) / (2.0 * q)
    a0 = 1.0 + alpha
    cw = math.cos(w0)
    return np.array([(1.0 + cw) / 2.0 / a0,
                     -(1.0 + cw) / a0,
                     (1.0 + cw) / 2.0 / a0,
                     -2.0 * cw / a0,
                     (1.0 - alpha) / a0], dtype=np.float64)


def rbj_bpf(f, fs, q):
    """The AR's bankRbjBpf: peak gain 1.0 at f0."""
    w0 = 2.0 * math.pi * f / fs
    alpha = math.sin(w0) / (2.0 * q)
    a0 = 1.0 + alpha
    return np.array([alpha / a0, 0.0, -alpha / a0,
                     -2.0 * math.cos(w0) / a0,
                     (1.0 - alpha) / a0], dtype=np.float64)


BUTTER_Q = 1.0 / math.sqrt(2.0)


def filter_designs():
    """The candidate band-pass filters, as lists of sections.

    'ideal' is the brick wall the algorithm was chosen with - it is the
    reference, not a candidate for the firmware.
    """
    return {
        "ideal":        None,
        # 2nd-order highpass at 40 Hz + 2nd-order lowpass at 160 Hz.
        "hp40_lp160":   [rbj_hpf(40.0, SR, BUTTER_Q), rbj_lpf(160.0, SR, BUTTER_Q)],
        # The same, but the lowpass twice: a 4th-order Butterworth.
        "hp40_lp160x2": [rbj_hpf(40.0, SR, BUTTER_Q),
                         rbj_lpf(160.0, SR, BUTTER_Q),
                         rbj_lpf(160.0, SR, BUTTER_Q)],
        # A lower top edge, to keep more of the snare's 185 Hz tone out.
        "hp40_lp140x2": [rbj_hpf(40.0, SR, BUTTER_Q),
                         rbj_lpf(140.0, SR, BUTTER_Q),
                         rbj_lpf(140.0, SR, BUTTER_Q)],
        # No highpass at all: the chain's own DC blocker already handles DC.
        "lp160x2":      [rbj_lpf(160.0, SR, BUTTER_Q),
                         rbj_lpf(160.0, SR, BUTTER_Q)],
        # One narrow band at 85 Hz, where a kick's fundamental lives.
        "bpf85":        [rbj_bpf(85.0, SR, 5.642), rbj_bpf(85.0, SR, 2.337)],
        # A wide band, centred where the kick's sweep passes.
        "bpf70":        [rbj_bpf(70.0, SR, 3.0), rbj_bpf(70.0, SR, 1.5)],
    }


def section_ir(c, n):
    """One section's impulse response, from the firmware's own recursion.

    The AR's transposed direct form II:
        y = b0*x + s1 ;  s1 = b1*x - a1*y + s2 ;  s2 = b2*x - a2*y
    A linear convolution with this impulse response IS what the firmware's
    recursion produces from a zero state - same coefficients, same filter,
    and no approximation.
    """
    b0, b1, b2, a1, a2 = c
    ir = np.zeros(n, dtype=np.float64)
    w1 = w2 = 0.0
    for i in range(n):
        x = 1.0 if i == 0 else 0.0
        o = b0 * x + w1
        w1 = b1 * x - a1 * o + w2
        w2 = b2 * x - a2 * o
        ir[i] = o
    return ir


def cascade_ir(sections, n=32768):
    """The cascade's impulse response: the sections in series."""
    ir = np.array([1.0])
    for c in sections:
        ir = np.convolve(ir, section_ir(c, n))[:n]
    return ir


def apply_sections(x, sections):
    """The cascade, by exact linear convolution with its impulse response."""
    if sections is None:
        return x.astype(np.float64)
    ir = cascade_ir(sections)
    n = len(x) + len(ir) - 1
    nfft = 1 << int(math.ceil(math.log2(n)))
    y = np.fft.irfft(np.fft.rfft(x, nfft) * np.fft.rfft(ir, nfft), nfft)[:len(x)]
    return y


# --------------------------------------------------------------------------
# The ODF
# --------------------------------------------------------------------------

def ideal_band(x, lo, hi):
    """The reference brick wall, for comparison only."""
    n = len(x)
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(n, 1.0 / SR)
    m = ((f >= lo) & (f < hi)).astype(np.float64)
    return np.fft.irfft(X * m, n=n)


def odf_from(x, sections, lo=40.0, hi=180.0):
    """The firmware's ODF: per-frame mean square, in dB, rectified difference."""
    y = apply_sections(x, sections) if sections is not None else ideal_band(x, lo, hi)
    n = (len(y) // HOP) * HOP
    frames = y[:n].reshape(-1, HOP)
    p = (frames.astype(np.float64) ** 2).mean(axis=1)
    db = 10.0 * np.log10(p + EPS)
    return np.maximum(np.diff(db, prepend=db[0]), 0.0)


# --------------------------------------------------------------------------
# The running DFT
# --------------------------------------------------------------------------

_TW = None


def twiddle():
    """The 4096-entry table: exp(-j*2*pi*m/4096), as (re, im) pairs."""
    global _TW
    if _TW is None:
        m = np.arange(NFFT, dtype=np.float64)
        ang = -2.0 * math.pi * m / NFFT
        _TW = np.stack([np.cos(ang), np.sin(ang)], axis=1)
    return _TW


def running_dft(odf):
    """The firmware's spectrum: one complex accumulator per in-band bin.

    Returns the magnitudes for bins K_MIN..K_MAX.
    """
    tw = twiddle()
    ks = np.arange(K_MIN, K_MAX + 1, dtype=np.int64)
    acc = np.zeros((N_BINS, 2), dtype=np.float64)

    n = len(odf)
    for i in range(n):
        # add the newest sample
        idx = (ks * (i & (NFFT - 1))) & (NFFT - 1)
        f = tw[idx]
        v = float(odf[i])
        acc[:, 0] += v * f[:, 0]
        acc[:, 1] += v * f[:, 1]
        # remove the one that just fell out of the window
        if i >= LEN:
            j = i - LEN
            idxo = (ks * (j & (NFFT - 1))) & (NFFT - 1)
            fo = tw[idxo]
            vo = float(odf[j])
            acc[:, 0] -= vo * fo[:, 0]
            acc[:, 1] -= vo * fo[:, 1]
    return np.sqrt(acc[:, 0] ** 2 + acc[:, 1] ** 2)


def numpy_spectrum(odf, demean=False):
    """The same thing done offline, for the check."""
    x = odf[-LEN:].astype(np.float64)
    if demean:
        x = x - x.mean()
    X = np.fft.rfft(x, NFFT)
    P = np.abs(X)
    return P[K_MIN:K_MAX + 1]


# --------------------------------------------------------------------------
# The selection rule
# --------------------------------------------------------------------------

def bin_bpm(k):
    return k * RATE / NFFT * 60.0


def peak_in(mags, kmin, kmax):
    """The tallest bin in the band, refined parabolically on the log."""
    sel = [k for k in range(kmin, kmax + 1)]
    if not sel:
        return None, 0.0
    # the magnitude array is indexed from K_MIN
    def mag(k):
        return mags[k - K_MIN]
    kbest = max(sel, key=lambda k: mag(k))
    bpm = bin_bpm(kbest)
    if K_MIN < kbest < K_MAX:
        a = math.log(mag(kbest - 1) + 1e-30)
        b = math.log(mag(kbest) + 1e-30)
        c = math.log(mag(kbest + 1) + 1e-30)
        den = a - 2.0 * b + c
        if abs(den) > 1e-12:
            d = 0.5 * (a - c) / den
            if abs(d) < 1.0:
                bpm += d * (RATE / NFFT * 60.0)
    return bpm, float(mag(kbest))


def local_peaks(mags, kmin, kmax):
    out = []
    for k in range(kmin, kmax + 1):
        if not (K_MIN < k < K_MAX):
            continue
        m = mags[k - K_MIN]
        if m >= mags[k - 1 - K_MIN] and m >= mags[k + 1 - K_MIN]:
            out.append((bin_bpm(k), float(m)))
    return out


def r2_3_2(mags, tol_db=3.0, max_ratio=4):
    """The rule: the tallest in-band peak, unless a lower one is within
    tol_db of it AND their ratio is a simple rational - then the lower one.

    The ratio test is what kills the 3:2 error (a 90 BPM track reading 135).
    """
    best, mag = peak_in(mags, K_MIN, K_MAX)
    if best is None:
        return None, 0.0
    thr = mag / (10.0 ** (tol_db / 20.0))
    for b, m in local_peaks(mags, K_MIN, K_MAX):
        if b >= best:
            continue
        if m <= 0.0 or m < thr:
            continue
        r = best / b
        for q in range(1, max_ratio + 1):
            for p in range(q + 1, max_ratio + 1):
                if abs(r - p / q) < 0.01 * (p / q):
                    return b, m
    return best, mag


# --------------------------------------------------------------------------
# The firmware's whole path, in one call
# --------------------------------------------------------------------------

def firmware_spectrum(odf):
    return running_dft(odf)


_TRACKS = {}


def track(style, bpm, human=0.0, noise=0.0):
    """The tracks are expensive to synthesise and are used by every design."""
    key = (style, bpm, human, noise)
    if key not in _TRACKS:
        _TRACKS[key] = make_track(bpm, style=style, seed=_seed(style, bpm),
                                  human=human, noise=noise)
    return _TRACKS[key]


def estimate(x, design="hp40_lp160x2", demean=False, skip=0.0):
    """The firmware's estimate for one track."""
    sections = filter_designs()[design]
    odf = odf_from(x, sections)
    if skip:
        odf = inject_skips(odf, RATE, skip)
    mags = firmware_spectrum(odf)
    bpm, score = r2_3_2(mags)
    return bpm, score, mags


# --------------------------------------------------------------------------
# The checks
# --------------------------------------------------------------------------

def check_running_dft():
    """The running DFT must agree with the offline FFT."""
    print("running DFT vs numpy, bins %d..%d of %d" % (K_MIN, K_MAX, NFFT))
    worst = 0.0
    for style in STYLES:
        x = track(style, 128)
        odf = odf_from(x, filter_designs()["hp40_lp160x2"])
        a = running_dft(odf)
        b = numpy_spectrum(odf, demean=False)
        rel = np.max(np.abs(a - b) / (b + 1e-30))
        worst = max(worst, float(rel))
        print("  %-10s max relative difference %.3e" % (style, rel))
    print("worst %.3e  %s" % (worst, "OK" if worst < 1e-4 else "TOO BIG"))
    return worst


def check_demean():
    """Does the running DFT want the window's mean removed?"""
    print("demean vs not, on the same tracks")
    for style in STYLES:
        x = track(style, 128)
        odf = odf_from(x, filter_designs()["hp40_lp160x2"])
        a = running_dft(odf)
        b = numpy_spectrum(odf, demean=True)
        print("  %-10s in-band floor %.3e  max %.3e  demeaned max %.3e"
              % (style, a[0], a.max(), b.max()))


def filter_shootout():
    """Which band-pass does the firmware use?"""
    bpms = (90, 100, 120, 128, 140, 150, 160, 174)
    print("%-14s %6s  %s" % ("design", "wrong", "per-style"))
    for name in filter_designs():
        counts = {}
        wrong = 0
        worst = 0.0
        for style in STYLES:
            c = 0
            for bpm in bpms:
                x = track(style, bpm)
                est, _, _ = estimate(x, name)
                if est is None:
                    c += 1
                    continue
                err = est - bpm
                worst = max(worst, abs(err))
                if abs(err) >= 2.0:
                    c += 1
            counts[style] = c
            wrong += c
        detail = " ".join("%s:%d" % (s[:4], c) for s, c in counts.items())
        print("%-14s %6d  %s   (worst error %.2f BPM)" % (name, wrong, detail, worst))


def matrix(design="hp40_lp160x2", human=0.0, noise=0.0, skip=0.0, quiet=False):
    bpms = (90, 100, 120, 128, 140, 150, 160, 174)
    if not quiet:
        print("design %s   human %.1f ms   noise %.3f   skips %.0f%%"
              % (design, human, noise, skip * 100))
        print("%-10s %6s %10s %8s %s" % ("style", "bpm", "estimated", "error", "verdict"))
    wrong = 0
    worst = 0.0
    counts = {}
    for style in STYLES:
        c = 0
        for bpm in bpms:
            x = track(style, bpm, human, noise)
            sections = filter_designs()[design]
            odf = odf_from(x, sections)
            if skip:
                odf = inject_skips(odf, RATE, skip)
            mags = running_dft(odf)
            est, _ = r2_3_2(mags)
            if est is None:
                if not quiet:
                    print("%-10s %6d %10s %8s %s" % (style, bpm, "-", "-", "NO ESTIMATE"))
                c += 1
                wrong += 1
                continue
            err = est - bpm
            worst = max(worst, abs(err))
            ok = abs(err) < 2.0
            if not quiet:
                print("%-10s %6d %10.2f %8.2f %s" % (
                    style, bpm, est, err, "ok" if ok else "WRONG"))
            if not ok:
                c += 1
                wrong += 1
        counts[style] = c
    if not quiet:
        print()
        print("%d wrong of %d   %s   worst error %.2f BPM"
              % (wrong, len(STYLES) * len(bpms),
                 " ".join("%s:%d" % (s[:4], v) for s, v in counts.items()), worst))
    return wrong, worst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--demean", action="store_true")
    ap.add_argument("--filters", action="store_true")
    ap.add_argument("--est", action="store_true")
    ap.add_argument("--design", default="hp40_lp160x2")
    ap.add_argument("--human", type=float, default=0.0)
    ap.add_argument("--noise", type=float, default=0.0)
    ap.add_argument("--skip", type=float, default=0.0)
    args = ap.parse_args()

    if args.check:
        check_running_dft()
    if args.demean:
        check_demean()
    if args.filters:
        filter_shootout()
    if args.est:
        matrix(args.design, args.human, args.noise, args.skip)
    if not (args.check or args.demean or args.filters or args.est):
        ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Experiment 4: the BAND decision, the tie-break rule, and the CONFIDENCE.

_tempo3_sim.py is the firmware-faithful model (causal filter, 93.75 Hz ODF,
running DFT over a 4096-point grid).  It found 5 wrong of 48, and two of
those five are BUGS in the model rather than failures of the method:

  1. the peak SEARCH was allowed to run into the +-1 margin bin, so a track
     could be reported OUTSIDE the band it was searched in (177.15 for a
     90 BPM track, with the band 90-175).  The margin bin exists for the
     parabolic refinement and must not be selectable.
  2. the tie-break demoted the winner to ANY lower peak whose ratio was a
     simple rational p/q with p,q <= 4, which included 4:3 - and a garage
     track at 150 was reported as 112.61 (= 150 * 3/4).

This file fixes (1), re-runs the band sweep, compares tie-break variants,
and measures the confidence features so the confidence can be designed from
data rather than from taste.

    python tools/ar_sim/_tempo4_sim.py --bands
    python tools/ar_sim/_tempo4_sim.py --conf
    python tools/ar_sim/_tempo4_sim.py --regress
"""

import argparse
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _tempo3_sim as T3                                    # noqa: E402
from _tempo2_sim import STYLES, make_track, inject_skips, _seed  # noqa: E402

SR = T3.SR
HOP = T3.HOP
RATE = T3.RATE
BPMS = (90, 100, 120, 128, 140, 150, 160, 174)
# the wider grid, for the sensitivity test: it deliberately includes the
# 3:2 partners of the band edges (135 is 3:2 of 90, 105 is 3:2 of 70,
# 120 is 3:2 of 80) and the half-time pairs (88/176, 85/170).
BPMS_WIDE = (88, 90, 92, 95, 100, 105, 110, 115, 120, 125, 128, 130,
             135, 140, 145, 150, 155, 160, 165, 170, 174, 176)
DESIGN = "hp40_lp160x2"

# --------------------------------------------------------------------------
# A running DFT with a configurable grid and band.
# --------------------------------------------------------------------------

_TW = {}


def twiddle(nfft):
    if nfft not in _TW:
        m = np.arange(nfft, dtype=np.float64)
        ang = -2.0 * math.pi * m / nfft
        _TW[nfft] = (np.cos(ang), np.sin(ang))
    return _TW[nfft]


def dft_mags(odf, nfft, length):
    """The magnitudes of the running DFT over the last `length` samples."""
    cos_t, sin_t = twiddle(nfft)
    n = len(odf)
    start = max(0, n - length)
    x = odf[start:]
    # the absolute index of the first sample of the window
    m0 = start
    # the window is the last `length` samples: index them from the end.
    acc = None
    # we add and remove one sample at a time, exactly as the firmware does.
    acc_re = None
    acc_im = None
    total_re = None
    total_im = None
    return None


def running_dft_bins(odf, nfft, kmin, kmax, length):
    """The magnitudes of the running DFT, over bins kmin..kmax.

    The firmware accumulates one complex accumulator per bin over a sliding
    window of `length` samples: add the newest, subtract the one that fell
    out.  That is EXACTLY the DFT of the last `length` samples zero-padded
    to `nfft`, and the magnitudes are identical (tempo3_sim --check proved
    the two agree to 2.3e-14).  So the simulation can use numpy's FFT and
    the firmware keeps the O(1) update.
    """
    x = odf[-length:].astype(np.float64)
    if len(x) < length:
        x = np.concatenate([np.zeros(length - len(x)), x])
    X = np.fft.rfft(x, nfft)
    P = np.abs(X)
    return P[kmin:kmax + 1], X[kmin:kmax + 1]


def bin_bpm(k, nfft):
    return k * RATE / nfft * 60.0


def band_bins(lo, hi, nfft):
    """The bins whose CENTRE is inside [lo, hi] - the selectable set."""
    k0 = int(math.ceil(lo / 60.0 * nfft / RATE))
    k1 = int(math.floor(hi / 60.0 * nfft / RATE))
    return k0, k1


def select(mags, kmin, kmax, nfft, lo, hi):
    """The tallest bin strictly inside the band, refined parabolically."""
    sel0, sel1 = band_bins(lo, hi, nfft)
    sel0 = max(sel0, kmin)
    sel1 = min(sel1, kmax)
    if sel1 < sel0:
        return None, 0.0, None
    sl = mags[sel0 - kmin: sel1 - kmin + 1]
    kbest = sel0 + int(np.argmax(sl))
    mag = float(sl[kbest - sel0])
    bpm = bin_bpm(kbest, nfft)
    # the parabolic refinement may use the margin bins, but the answer is
    # clamped to the band.
    if kmin < kbest < kmax:
        a = math.log(max(mags[kbest - 1 - kmin], 1e-30))
        b = math.log(max(mag, 1e-30))
        c = math.log(max(mags[kbest + 1 - kmin], 1e-30))
        den = a - 2.0 * b + c
        if abs(den) > 1e-12:
            d = 0.5 * (a - c) / den
            if abs(d) < 1.0:
                bpm += d * (RATE / nfft * 60.0)
    bpm = min(max(bpm, lo), hi)
    return bpm, mag, kbest


def local_peaks(mags, kmin, kmax, nfft, lo, hi):
    sel0, sel1 = band_bins(lo, hi, nfft)
    out = []
    for k in range(max(kmin + 1, sel0), min(kmax - 1, sel1) + 1):
        m = mags[k - kmin]
        if m >= mags[k - 1 - kmin] and m >= mags[k + 1 - kmin]:
            out.append((bin_bpm(k, nfft), float(m), k))
    return out


_NFFT_HOLDER = {}


def nfft_of(mags):
    return _NFFT_HOLDER["nfft"]


# --------------------------------------------------------------------------
# Tie-break variants.
# --------------------------------------------------------------------------

def tie_none(mags, kmin, kmax, nfft, lo, hi):
    return select(mags, kmin, kmax, nfft, lo, hi)


def _demote(mags, kmin, kmax, nfft, lo, hi, ratios, tol_db):
    bpm, mag, kbest = select(mags, kmin, kmax, nfft, lo, hi)
    if bpm is None:
        return None, 0.0, None
    thr = mag / (10.0 ** (tol_db / 20.0))
    for pb, pm, kp in local_peaks(mags, kmin, kmax, nfft, lo, hi):
        if pb >= bpm - 1e-6:
            continue
        if pm <= 0 or pm < thr:
            continue
        r = bpm / pb
        for (p, q) in ratios:
            if abs(r - p / q) < 0.01 * (p / q):
                return pb, pm, kp
    return bpm, mag, kbest


def tie_32(mags, kmin, kmax, nfft, lo, hi):
    """Demote only to a 3:2 partner (the dotted-quarter reading)."""
    return _demote(mags, kmin, kmax, nfft, lo, hi, [(3, 2)], 3.0)


def tie_32_21(mags, kmin, kmax, nfft, lo, hi):
    """Demote to a 3:2 OR a 2:1 partner.

    The 2:1 case is the octave: a track whose real tempo is below the
    band's floor has its double inside the band, and the double is what
    wins.  Demoting it puts the slower reading back - which is right for
    a track that really is slow, and wrong for a track that really is
    double-time.  Whether that trade is worth making is measured, not
    assumed.
    """
    return _demote(mags, kmin, kmax, nfft, lo, hi, [(3, 2), (2, 1)], 3.0)


def tie_21(mags, kmin, kmax, nfft, lo, hi):
    """Demote to a 2:1 partner only."""
    return _demote(mags, kmin, kmax, nfft, lo, hi, [(2, 1)], 3.0)


def tie_32_21_43(mags, kmin, kmax, nfft, lo, hi):
    """Demote to a 3:2, a 2:1 or a 4:3 partner."""
    return _demote(mags, kmin, kmax, nfft, lo, hi,
                   [(3, 2), (2, 1), (4, 3)], 3.0)


def tie_32up(mags, kmin, kmax, nfft, lo, hi):
    """The other direction: promote to the 3:2 partner that is FASTER.

    Club music is catalogued at the FAST level of a 3:2 pair (a 100 BPM
    track that also has a 150 BPM 3-against-2 grid is a 150 BPM track).
    This rule promotes; tie_32 demotes.  Which is right is an empirical
    question, so both are measured.
    """
    bpm, mag, kbest = select(mags, kmin, kmax, nfft, lo, hi)
    if bpm is None:
        return None, 0.0, None
    thr = mag / (10.0 ** (3.0 / 20.0))
    for pb, pm, kp in local_peaks(mags, kmin, kmax, nfft, lo, hi):
        if pb <= bpm + 1e-6:
            continue
        if pm <= 0 or pm < thr:
            continue
        r = pb / bpm
        if abs(r - 3.0 / 2.0) < 0.01 * 1.5:
            return pb, pm, kp
    return bpm, mag, kbest


def tie_32_43(mags, kmin, kmax, nfft, lo, hi):
    """Demote to a 3:2 or a 4:3 partner."""
    return _demote(mags, kmin, kmax, nfft, lo, hi, [(3, 2), (4, 3)], 3.0)


def tie_all4(mags, kmin, kmax, nfft, lo, hi):
    """The research rule: any p/q with p,q <= 4."""
    return _demote(mags, kmin, kmax, nfft, lo, hi,
                   [(p, q) for q in range(1, 5) for p in range(q + 1, 5)], 3.0)


TIES = {
    "none": tie_none,
    "32": tie_32,
    "32up": tie_32up,
    "21": tie_21,
    "32+21": tie_32_21,
    "32+21+43": tie_32_21_43,
    "32+43": tie_32_43,
    "all4": tie_all4,
}


# --------------------------------------------------------------------------
# The tracks.
# --------------------------------------------------------------------------

_ODF = {}
_IR = {}


def cascade_ir_fft(sections, n=8192):
    """The cascade's impulse response, by FFT (np.convolve of two 32768
    arrays is O(n^2) and is what makes the sweep unbearably slow)."""
    ir = np.array([1.0])
    for c in sections:
        s = T3.section_ir(c, n)
        m = 1 << int(math.ceil(math.log2(len(ir) + len(s) - 1)))
        ir = np.fft.irfft(np.fft.rfft(ir, m) * np.fft.rfft(s, m), m)[:n]
    return ir


def apply_fast(x, sections):
    """The cascade, by convolution with its impulse response.

    The impulse response is cached per FILTER, not per design name: the
    decimated path uses three different cascades and a shared key would
    hand each of them the first one's response.
    """
    key = tuple(tuple(c) for c in sections)
    ir = _IR.get(key)
    if ir is None:
        ir = cascade_ir_fft(sections)
        _IR[key] = ir
    n = len(x) + len(ir) - 1
    m = 1 << int(math.ceil(math.log2(n)))
    return np.fft.irfft(np.fft.rfft(x, m) * np.fft.rfft(ir, m), m)[:len(x)]


def odf_fast(x, sections):
    """The firmware's ODF: per-frame mean square, in dB, rectified difference."""
    y = apply_fast(x, sections)
    n = (len(y) // HOP) * HOP
    frames = y[:n].reshape(-1, HOP)
    p = (frames.astype(np.float64) ** 2).mean(axis=1)
    db = 10.0 * np.log10(p + T3.EPS)
    return np.maximum(np.diff(db, prepend=db[0]), 0.0)


def odf_for(style, bpm, human=0.0, noise=0.0):
    key = (style, bpm, human, noise)
    if key not in _ODF:
        x = make_track(bpm, style=style, seed=_seed(style, bpm),
                       human=human, noise=noise)
        sections = T3.filter_designs()[DESIGN]
        _ODF[key] = odf_fast(x, sections)
    return _ODF[key]


# --------------------------------------------------------------------------
# The sweep.
# --------------------------------------------------------------------------

def classify(est, truth):
    if est is None:
        return "none"
    if abs(est - truth) < 2.0:
        return "ok"
    for name, mult in (("x2", 2.0), ("/2", 0.5), ("x3", 3.0), ("/3", 1.0 / 3),
                        ("3:2", 1.5), ("2:3", 2.0 / 3), ("4:3", 4.0 / 3),
                        ("3:4", 3.0 / 4)):
        if abs(est - truth * mult) < max(2.0, truth * mult * 0.02):
            return name
    return "other"


def sweep(nfft=4096, length=1125, human=0.0, noise=0.0, skip=0.0):
    bands = [(90, 150), (90, 160), (90, 175), (95, 175), (100, 170),
             (110, 175), (120, 176), (120, 180), (118, 176), (100, 160),
             (70, 175), (70, 180), (80, 176)]
    # one DFT per track, wide enough for the widest band, then every band
    # and every tie-break is evaluated from the same magnitudes.
    gmin = min(b[0] for b in bands)
    gmax = max(b[1] for b in bands)
    kmin = int(math.floor(gmin / 60.0 * nfft / RATE)) - 1
    kmax = int(math.ceil(gmax / 60.0 * nfft / RATE)) + 1
    print("nfft %d  window %.1f s  human %.0f ms  noise %.3f  skips %.0f%%"
          % (nfft, length / RATE, human, noise, skip * 100))
    print("bins %d..%d (%.2f .. %.2f BPM)   grid %.3f BPM"
          % (kmin, kmax, bin_bpm(kmin, nfft), bin_bpm(kmax, nfft),
             RATE / nfft * 60.0))
    # one DFT per track, cached, over the widest band
    cache = {}
    for style in STYLES:
        for bpm in BPMS:
            odf = odf_for(style, bpm, human, noise)
            if skip:
                odf = inject_skips(odf, RATE, skip)
            re, im = running_dft_bins(odf, nfft, kmin, kmax, length)
            cache[(style, bpm)] = re
    print("%-11s %-7s %6s %6s  %-22s %s"
          % ("band", "tie", "wrong", "cover", "breakdown", "note"))
    for (lo, hi) in bands:
        for tname, tfn in TIES.items():
            wrong = 0
            total = 0
            kinds = {}
            worst = 0.0
            for style in STYLES:
                for bpm in BPMS:
                    if not (lo <= bpm <= hi):
                        # the truth is OUTSIDE the band: the estimate cannot
                        # be right, and that is the band's doing, not the
                        # estimator's.  Counted separately.
                        continue
                    total += 1
                    mags = cache[(style, bpm)]
                    est, mag, kb = tfn(mags, kmin, kmax, nfft, lo, hi)
                    if est is None:
                        wrong += 1
                        kinds["none"] = kinds.get("none", 0) + 1
                        continue
                    k = classify(est, bpm)
                    if k != "ok":
                        wrong += 1
                        kinds[k] = kinds.get(k, 0) + 1
                        worst = max(worst, abs(est - bpm))
            print("%-11s %-7s %6d %6d  %-22s worst %.1f"
                  % ("%d-%d" % (lo, hi), tname, wrong, total,
                     " ".join("%s:%d" % (a, b) for a, b in sorted(kinds.items())),
                     worst))
    return 0


# --------------------------------------------------------------------------
# The confidence features.
# --------------------------------------------------------------------------

def conf_features(nfft=4096, length=1125):
    """The peak-to-background ratio and the half-window agreement."""
    lo, hi = 90.0, 175.0
    kmin = int(math.floor(lo / 60.0 * nfft / RATE)) - 1
    kmax = int(math.ceil(hi / 60.0 * nfft / RATE)) + 1
    print("%-9s %6s %9s %8s %8s %8s" %
          ("style", "bpm", "est", "snr_db", "halfok", "snr2_db"))
    rows = []
    for style in STYLES:
        for bpm in BPMS:
            odf = odf_for(style, bpm)
            re, im = running_dft_bins(odf, nfft, kmin, kmax, length)
            mags = re
            est, mag, kb = tie_32(mags, kmin, kmax, nfft, lo, hi)
            # the background: the median of the in-band magnitudes, with a
            # guard of +-3 bins around the winner
            s0, s1 = band_bins(lo, hi, nfft)
            guard = [k - kmin for k in range(s0, s1 + 1)
                     if kb is None or abs(k - kb) > 3]
            bg = float(np.median(mags[guard])) if guard else 0.0
            snr = 20.0 * math.log10((mag + 1e-30) / (bg + 1e-30))
            # the same peak, measured on the second half of the window only
            half = length // 2
            odf2 = odf[len(odf) - half:]
            re2, im2 = running_dft_bins(odf2, nfft, kmin, kmax, half)
            mags2 = re2
            est2, mag2, kb2 = tie_32(mags2, kmin, kmax, nfft, lo, hi)
            halfok = 1 if (kb2 is not None and kb is not None
                           and abs(est2 - est) < 2.0) else 0
            guard2 = [k - kmin for k in range(s0, s1 + 1)
                      if kb2 is None or abs(k - kb2) > 3]
            bg2 = float(np.median(mags2[guard2])) if guard2 else 0.0
            snr2 = 20.0 * math.log10((mag2 + 1e-30) / (bg2 + 1e-30))
            ok = classify(est, bpm) == "ok"
            print("%-9s %6d %9.2f %8.2f %8d %8.2f  %s" %
                  (style, bpm, est, snr, halfok, snr2, "ok" if ok else "WRONG"))
            rows.append((style, bpm, est, snr, halfok, snr2, ok))
    ok_rows = [r for r in rows if r[6]]
    bad_rows = [r for r in rows if not r[6]]
    if ok_rows:
        print("\ncorrect : snr_db %.1f..%.1f (median %.1f)  halfok %d/%d" %
              (min(r[3] for r in ok_rows), max(r[3] for r in ok_rows),
               float(np.median([r[3] for r in ok_rows])),
               sum(r[4] for r in ok_rows), len(ok_rows)))
    if bad_rows:
        print("wrong   : snr_db %.1f..%.1f (median %.1f)  halfok %d/%d" %
              (min(r[3] for r in bad_rows), max(r[3] for r in bad_rows),
               float(np.median([r[3] for r in bad_rows])),
               sum(r[4] for r in bad_rows), len(bad_rows)))
    return rows


# --------------------------------------------------------------------------
# The beat-interval regression: the refinement that is not quantised.
# --------------------------------------------------------------------------

def regress(odf, rate, lo, hi):
    """Peak-pick the ODF, take the median interval, then fit beat index."""
    med = float(np.median(odf))
    mad = float(np.median(np.abs(odf - med))) + 1e-12
    thr = med + 3.0 * mad
    onsets = []
    for i in range(1, len(odf) - 1):
        if odf[i] >= thr and odf[i] > odf[i - 1] and odf[i] >= odf[i + 1]:
            onsets.append(i)
    if len(onsets) < 8:
        return None, None, len(onsets)
    # the median interval is the robust period
    d = np.diff(np.array(onsets, dtype=np.float64))
    d = d[d > 1]
    if len(d) == 0:
        return None, None, len(onsets)
    per = float(np.median(d))
    # the least squares fit of the beat index against the onset time
    idx = np.arange(len(onsets), dtype=np.float64)
    t = np.array(onsets, dtype=np.float64)
    A = np.vstack([t, np.ones_like(t)]).T
    sol, res, _rank, _sv = np.linalg.lstsq(A, idx, rcond=None)
    slope = sol[0]
    if slope <= 0:
        return per, None, len(onsets)
    return per, 1.0 / (slope / rate), len(onsets)


def run_regress():
    print("%-9s %6s %10s %10s %8s %6s" %
          ("style", "bpm", "median_int", "fit_bpm", "err", "n"))
    errs = []
    for style in STYLES:
        for bpm in BPMS:
            odf = odf_for(style, bpm)
            per, fit, n = regress(odf, RATE, 90, 175)
            if fit is None:
                print("%-9s %6d %10s %10s %8s %6d" % (style, bpm, "-", "-", "-", n))
                continue
            err = fit - bpm
            errs.append(abs(err))
            print("%-9s %6d %10.3f %10.3f %8.3f %6d" %
                  (style, bpm, per, fit, err, n))
    if errs:
        e = np.array(errs)
        print("\n|err| median %.3f  p90 %.3f  max %.3f  (BPM)" %
              (float(np.median(e)), float(np.percentile(e, 90)), float(e.max())))


def detail(lo=90, hi=150, tie="32", nfft=4096, length=1125,
           human=0.0, noise=0.0, skip=0.0):
    """Every track, every estimate, in band or not."""
    tfn = TIES[tie]
    gmin = min(lo, 60)
    kmin = int(math.floor(gmin / 60.0 * nfft / RATE)) - 1
    kmax = int(math.ceil(hi / 60.0 * nfft / RATE)) + 1
    print("band %d-%d  tie %s  window %.1f s  human %.0f ms  noise %.3f  skip %.2f"
          % (lo, hi, tie, length / RATE, human, noise, skip))
    print("%-9s %6s %9s %7s %-7s" % ("style", "bpm", "est", "inband", "kind"))
    for style in STYLES:
        for bpm in BPMS:
            odf = odf_for(style, bpm, human, noise)
            if skip:
                odf = inject_skips(odf, RATE, skip)
            re, im = running_dft_bins(odf, nfft, kmin, kmax, length)
            est, mag, kb = tfn(re, kmin, kmax, nfft, lo, hi)
            inb = "yes" if est is not None and lo <= est <= hi else "NO"
            print("%-9s %6d %9s %7s %-7s"
                  % (style, bpm, ("%.2f" % est) if est else "-", inb,
                     classify(est, bpm)))
    return 0


# --------------------------------------------------------------------------
# Robustness: the conditions a real track has and a synthetic one does not.
# --------------------------------------------------------------------------

def robust(lo=90, hi=176, tie="32", nfft=4096, length=1125):
    tfn = TIES[tie]
    kmin = int(math.floor(lo / 60.0 * nfft / RATE)) - 1
    kmax = int(math.ceil(hi / 60.0 * nfft / RATE)) + 1
    cases = [("clean", 0.0, 0.0, 0.0),
             ("human 25 ms", 25.0, 0.0, 0.0),
             ("human 50 ms", 50.0, 0.0, 0.0),
             ("noise 0.02", 0.0, 0.02, 0.0),
             ("noise 0.05", 0.0, 0.05, 0.0),
             ("noise 0.10", 0.0, 0.10, 0.0),
             ("skip 25%", 0.0, 0.0, 0.25),
             ("skip 50%", 0.0, 0.0, 0.50),
             ("all three", 25.0, 0.02, 0.25)]
    print("band %d-%d  tie %s" % (lo, hi, tie))
    print("%-14s %6s %6s  %s" % ("case", "wrong", "cover", "breakdown"))
    for (name, human, noise, skip) in cases:
        wrong = 0
        total = 0
        kinds = {}
        worst = 0.0
        for style in STYLES:
            for bpm in BPMS:
                if not (lo <= bpm <= hi):
                    continue
                total += 1
                odf = odf_for(style, bpm, human, noise)
                if skip:
                    odf = inject_skips(odf, RATE, skip)
                re, im = running_dft_bins(odf, nfft, kmin, kmax, length)
                est, mag, kb = tfn(re, kmin, kmax, nfft, lo, hi)
                if est is None:
                    wrong += 1
                    kinds["none"] = kinds.get("none", 0) + 1
                    continue
                k = classify(est, bpm)
                if k != "ok":
                    wrong += 1
                    kinds[k] = kinds.get(k, 0) + 1
                    worst = max(worst, abs(est - bpm))
                else:
                    worst = max(worst, abs(est - bpm))
        print("%-14s %6d %6d  %-22s worst %.2f" %
              (name, wrong, total,
               " ".join("%s:%d" % (a, b) for a, b in sorted(kinds.items())),
               worst))
    return 0


# --------------------------------------------------------------------------
# The sub-frame onset position: the phase detector's timing.
# --------------------------------------------------------------------------

def onset_3k(x, sections, dec=16):
    """The firmware's tempo path, at 3 kHz.

    The band-passed signal, decimated to 3 kHz, with a per-sample onset
    strength: the half-wave-rectified difference of the log envelope.
    Returns (onset[n] at 3 kHz, times in seconds).
    """
    y = apply_fast(x, sections)
    # decimate by 16 (the bank does the same); the band-pass already killed
    # everything above 160 Hz, so this is alias-free.
    n = (len(y) // dec) * dec
    y = y[:n].reshape(-1, dec).mean(axis=1)
    p = y.astype(np.float64) ** 2
    db = 10.0 * np.log10(p + 1e-12)
    # a 5 ms reference at 3 kHz is 15 samples
    a = 1.0 - math.exp(-1.0 / (0.005 * 3000.0))
    ref = 0.0
    out = np.zeros(len(db))
    for i in range(len(db)):
        d = max(0.0, db[i] - ref)
        ref += a * (db[i] - ref)
        out[i] = d
    return out, np.arange(len(db)) / 3000.0


def phase_test():
    """How well does the onset position locate the beat?"""
    sections = T3.filter_designs()[DESIGN]
    print("%-9s %6s %10s %10s %10s" %
          ("style", "bpm", "onset_err_ms", "n", "hop_err_ms"))
    errs = []
    for style in STYLES:
        for bpm in BPMS:
            x = make_track(bpm, style=style, seed=_seed(style, bpm))
            o, t = onset_3k(x, sections)
            # the true beat times
            beat = 60.0 / bpm
            nb = int(t[-1] / beat)
            tru = np.arange(nb) * beat
            # the detected onsets: the local maxima above the median
            med = np.median(o)
            peaks = []
            i = 1
            while i < len(o) - 1:
                if o[i] >= o[i - 1] and o[i] > o[i + 1] and o[i] > med * 2:
                    peaks.append(t[i])
                    i += 12
                else:
                    i += 1
            if len(peaks) < 4:
                print("%-9s %6d %10s %10d" % (style, bpm, "-", len(peaks)))
                continue
            peaks = np.array(peaks)
            # the nearest true beat for each detected onset
            e = []
            for p in peaks:
                j = int(round(p / beat))
                if 0 <= j < len(tru):
                    e.append((p - tru[j]) * 1000.0)
            if not e:
                continue
            e = np.array(e)
            errs.append(np.median(np.abs(e)))
            print("%-9s %6d %10.2f %10d" % (style, bpm, np.median(np.abs(e)),
                                            len(e)))
    if errs:
        e = np.array(errs)
        print("\nmedian |onset error| : %.2f ms  (the frame is 10.67 ms)"
              % float(np.median(e)))
    return 0


# --------------------------------------------------------------------------
# The decimated path: what the firmware can actually afford.
# --------------------------------------------------------------------------

def odf_decimated(x, sections, dec=16):
    """The firmware's ODF, from a decimated band-passed signal.

    The AR's bottom bank already decimates 48 kHz to 3 kHz (AR_BANK_DEC=16)
    behind a 400 Hz anti-alias filter.  A tempo path that reuses that rate
    gets 32 samples a frame, not 512, so the ODF is 16 times cheaper.

    The question this answers: does decimating to 3 kHz BEFORE the band-pass
    cost accuracy?  The band is 40-160 Hz, so 3 kHz is 18 times the top of
    the band - but the anti-alias filter has to be right, and 40-160 Hz
    content aliases to 0-160 Hz only from 3-3.16 kHz.
    """
    y = apply_fast(x, sections)
    n = (len(y) // dec) * dec
    # the bank's anti-alias filter: a 4th-order Butterworth at 400 Hz
    aa = [T3.rbj_lpf(400.0, SR, T3.BUTTER_Q), T3.rbj_lpf(400.0, SR, T3.BUTTER_Q)]
    y = apply_fast(y, aa)
    y = y[:n].reshape(-1, dec).mean(axis=1)
    # the tempo band-pass, now at 3 kHz
    lo = [T3.rbj_hpf(40.0, 3000.0, T3.BUTTER_Q)]
    hi = [T3.rbj_lpf(160.0, 3000.0, T3.BUTTER_Q),
          T3.rbj_lpf(160.0, 3000.0, T3.BUTTER_Q)]
    y = apply_fast(y, lo)
    y = apply_fast(y, hi)
    p = y.astype(np.float64) ** 2
    db = 10.0 * np.log10(p + T3.EPS)
    return np.maximum(np.diff(db, prepend=db[0]), 0.0)


def decimated_test(lo=90, hi=176, tie="32", nfft=4096, length=1125):
    """Does the cheap decimated path give the same answer as the 48 kHz one?"""
    tfn = TIES[tie]
    sections = T3.filter_designs()[DESIGN]
    kmin = int(math.floor(lo / 60.0 * nfft / RATE)) - 1
    kmax = int(math.ceil(hi / 60.0 * nfft / RATE)) + 1
    print("%-9s %6s %10s %10s" % ("style", "bpm", "48k_bpm", "3k_bpm"))
    same = 0
    tot = 0
    for style in STYLES:
        for bpm in BPMS:
            x = make_track(bpm, style=style, seed=_seed(style, bpm))
            o1 = odf_fast(x, sections)
            o2 = odf_decimated(x, sections)
            r1, i1 = running_dft_bins(o1, nfft, kmin, kmax, length)
            r2, i2 = running_dft_bins(o2, nfft, kmin, kmax, length)
            e1, _, _ = tfn(r1, kmin, kmax, nfft, lo, hi)
            e2, _, _ = tfn(r2, kmin, kmax, nfft, lo, hi)
            tot += 1
            ok = (e1 is not None and e2 is not None
                  and abs(e1 - e2) < 1.0)
            same += 1 if ok else 0
            print("%-9s %6d %10s %10s %s" %
                  (style, bpm, "%.2f" % e1 if e1 else "-",
                   "%.2f" % e2 if e2 else "-", "" if ok else "DIFFER"))
    print("\nagree %d of %d" % (same, tot))
    return 0


# --------------------------------------------------------------------------
# The wide grid: which BPMs break the band.
# --------------------------------------------------------------------------

def wide_test(lo=90, hi=176, tie="32", nfft=4096, length=1125):
    tfn = TIES[tie]
    kmin = int(math.floor(lo / 60.0 * nfft / RATE)) - 1
    kmax = int(math.ceil(hi / 60.0 * nfft / RATE)) + 1
    print("band %d-%d  tie %s" % (lo, hi, tie))
    print("%-9s %6s %10s %-8s %8s" % ("style", "bpm", "est", "kind", "err"))
    for style in STYLES:
        for bpm in BPMS_WIDE:
            odf = odf_for(style, bpm)
            re, im = running_dft_bins(odf, nfft, kmin, kmax, length)
            est, mag, kb = tfn(re, kmin, kmax, nfft, lo, hi)
            k = classify(est, bpm)
            print("%-9s %6d %10s %-8s %8.2f" %
                  (style, bpm, "%.2f" % est if est else "-", k,
                   (est - bpm) if est else 0.0))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bands", action="store_true")
    ap.add_argument("--conf", action="store_true")
    ap.add_argument("--regress", action="store_true")
    ap.add_argument("--detail", action="store_true")
    ap.add_argument("--robust", action="store_true")
    ap.add_argument("--phase", action="store_true")
    ap.add_argument("--decimated", action="store_true")
    ap.add_argument("--wide", action="store_true")
    ap.add_argument("--lo", type=float, default=90.0)
    ap.add_argument("--hi", type=float, default=176.0)
    ap.add_argument("--tie", default="32")
    ap.add_argument("--nfft", type=int, default=4096)
    ap.add_argument("--window", type=float, default=12.0)
    ap.add_argument("--human", type=float, default=0.0)
    ap.add_argument("--noise", type=float, default=0.0)
    ap.add_argument("--skip", type=float, default=0.0)
    args = ap.parse_args()
    _NFFT_HOLDER["nfft"] = args.nfft
    if args.bands:
        sweep(args.nfft, int(args.window * RATE), args.human, args.noise, args.skip)
    if args.conf:
        conf_features(args.nfft, int(args.window * RATE))
    if args.regress:
        run_regress()
    if args.detail:
        detail(args.lo, args.hi, args.tie, args.nfft,
               int(args.window * RATE), args.human, args.noise, args.skip)
    if args.robust:
        robust(args.lo, args.hi, args.tie, args.nfft, int(args.window * RATE))
    if args.phase:
        phase_test()
    if args.decimated:
        decimated_test(args.lo, args.hi, args.tie, args.nfft,
                       int(args.window * RATE))
    if args.wide:
        wide_test(args.lo, args.hi, args.tie, args.nfft,
                  int(args.window * RATE))
    if not (args.bands or args.conf or args.regress or args.detail
            or args.robust or args.phase):
        ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())

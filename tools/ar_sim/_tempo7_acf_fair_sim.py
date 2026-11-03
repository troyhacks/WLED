#!/usr/bin/env python3
"""Is the ACF's loss real, or is it my ACF?

The brief asked for an autocorrelation tempo estimate.  My first attempt at
one lost badly to a spectral peak (23 of 54 wrong against 0 of 54).  Before
designing around that, the ACF has to be given its best shot: the classical
forms, each with the freedom to pick its own normalisation, comb and prior.

If the BEST of them still loses, the deviation is justified by measurement.
If one of them wins, the design should use it.

Variants:
  raw      r[L], no normalisation
  overlap  r[L]/(N-L)          - the unbiased estimator for a constant signal
  r0       r[L]/r[0]           - the normalised ACF
  nacf     r[L]/(N-L) after removing the mean, i.e. the same as overlap
  comb     Davies' ACC: sum_m w[m]*r[m*L], with w[m] = 1/m
  combN    the same, with the comb taps normalised by the overlap count
  pri      any of the above times a log-normal tempo prior (Ellis 2007)
  dp       the tempogram of the ACF, i.e. the DFT of the ACF (the "beat
           spectrum"), which is what a lot of papers actually call the ACF
           method
and the estimator is the argmax over the band, with the parabolic refinement
and the same 3:2 tie-break the spectral route gets.

Run: python tools/ar_sim/_tempo7_acf_fair_sim.py
"""

import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _tempo2_sim import STYLES, _seed, make_track   # noqa: E402
from _tempo5_lock_sim import (RATE, LEN, TRUTHS, odf_of, _parabolic,   # noqa: E402
                               bin_range, tie_break)
from _tempo6_final_sim import spectral, KICK, spec_of   # noqa: E402

BPM_LO, BPM_HI = 90.0, 175.0


def acf_score(odf, kind, prior_sigma=1.4, prior_centre=0.5):
    """The ACF variant's score over the band."""
    x = odf[-LEN:].astype(np.float64)
    x = x - x.mean()
    n = len(x)
    r = np.correlate(x, x, mode="full")[n - 1:]
    lag_lo = max(2, int(math.floor(60.0 * RATE / BPM_HI)))
    lag_hi = min(n - 3, int(math.ceil(60.0 * RATE / BPM_LO)))
    lags = np.arange(lag_lo, lag_hi + 1)
    if kind == "raw":
        s = r[lags].astype(np.float64)
    elif kind == "overlap":
        s = r[lags] / (n - lags)
    elif kind == "r0":
        s = r[lags] / max(r[0], 1e-20)
    elif kind == "comb":
        s = np.zeros(len(lags))
        for m in range(1, 5):
            idx = np.clip(lags * m - lag_lo, 0, len(r) - 1)
            valid = (lags * m) < len(r)
            s += np.where(valid, r[np.clip(lags * m, 0, len(r) - 1)] / m, 0.0)
    elif kind == "combN":
        s = np.zeros(len(lags))
        for m in range(1, 5):
            idx = np.clip(lags * m - lag_lo, 0, len(r) - 1)
            valid = (lags * m) < len(r)
            s += np.where(valid, r[np.clip(lags * m, 0, len(r) - 1)] / (m * (n - lags * m)), 0.0)
    elif kind == "dp":
        # the tempogram: the magnitude spectrum of the ACF, sampled at the
        # band's frequencies.  This is what "the ACF method" means in a lot
        # of the literature.
        F = np.abs(np.fft.rfft(r, 8192))
        fr = np.fft.rfftfreq(8192, 1.0 / n)
        bpm_of = fr * 60.0 * RATE
        s = np.interp(60.0 * RATE / lags, bpm_of, F)
    else:
        raise ValueError(kind)
    if prior_sigma > 0:
        theta0 = prior_centre * RATE
        prior = np.exp(-0.5 * (np.log2(lags / theta0) / prior_sigma) ** 2)
        s = s * prior
    return lags, s


def acf_estimate(odf, kind, prior_sigma=1.4, tol_db=3.0):
    lags, s = acf_score(odf, kind, prior_sigma)
    k = int(np.argmax(s))
    lag = float(lags[k])
    if 0 < k < len(s) - 1:
        d = _parabolic(s, k)
        if d:
            lag += d
    bpm = 60.0 * RATE / lag
    # the same 3:2 tie-break, over the ACF's own local peaks
    peaks = []
    for i in range(1, len(s) - 1):
        if s[i] >= s[i - 1] and s[i] > s[i + 1]:
            peaks.append((60.0 * RATE / lags[i], float(s[i])))
    best_mag = float(s[k])
    bpm, best_mag, _ = tie_break(bpm, best_mag, peaks, tol_db, ((3, 2),))
    return bpm


def main():
    # (the ACF variant, the prior's sigma in octaves, the label)
    # The label is separate from the variant: the variant is what acf_score()
    # knows how to compute, and the label is what the table prints.
    variants = [
        ("raw", 0.0, "raw"), ("overlap", 0.0, "overlap"),
        ("r0", 0.0, "r0"),
        ("comb", 0.0, "comb"), ("combN", 0.0, "combN"),
        ("overlap", 1.4, "overlap+prior1.4"),
        ("comb", 1.4, "comb+prior1.4"),
        ("comb", 0.8, "comb+prior0.8"),
        ("comb", 0.5, "comb+prior0.5"),
        ("overlap", 0.8, "overlap+prior0.8"),
        ("overlap", 0.5, "overlap+prior0.5"),
        ("dp", 0.0, "dp"),
        ("dp", 1.4, "dp+prior1.4"),
    ]
    print("=== the ACF's best shot, against the spectral peak ===")
    print("the same ODF, the same window, the same 3:2 tie-break")
    print()
    print("%-16s %6s %9s %9s" % ("variant", "wrong", "maxerr", "rmserr"))
    for kind, ps, label in variants:
        errs = []
        wrong = 0
        worst = ""
        for style in STYLES:
            for bpm in TRUTHS:
                odf = odf_of(KICK, style, bpm)
                e = acf_estimate(odf, kind, ps)
                err = e - bpm
                errs.append(err)
                if abs(err) >= 2.0:
                    wrong += 1
                    worst = "%s %d -> %.1f" % (style, bpm, e)
        errs = np.array(errs)
        print("%-16s %6d %9.2f %9.2f  %s" %
              (label, wrong,
               np.max(np.abs(errs)), np.sqrt(np.mean(errs ** 2)), worst))
    # the spectral route, for the same rows
    errs = []
    wrong = 0
    for style in STYLES:
        for bpm in TRUTHS:
            odf = odf_of(KICK, style, bpm)
            e, m, edge = spectral(odf)
            err = e - bpm
            errs.append(err)
            if abs(err) >= 2.0:
                wrong += 1
    errs = np.array(errs)
    print("%-16s %6d %9.2f %9.2f" % ("SPECTRAL", wrong,
                                     np.max(np.abs(errs)),
                                     np.sqrt(np.mean(errs ** 2))))
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())

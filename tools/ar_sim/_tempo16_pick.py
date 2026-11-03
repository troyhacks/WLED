#!/usr/bin/env python3
"""Two questions the design has to answer:

  1. The brief asks for PEAK-PICKED ONSETS.  Does peak-picking help the
     spectral route, hurt it, or do nothing?  The spectral route was measured
     on the RAW rectified dB difference; if peak-picking is not measured to
     help, it is not shipped (minimal-but-correct).

  2. The phase comb's bin count, measured on the ESTIMATED tempo (not the
     truth), with the signed bias, so the firmware's phase-bias setting is
     derived from a number rather than guessed.

Run: python tools/ar_sim/_tempo16_pick.py
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _tempo5_lock_sim import RATE, odf_of, STYLES           # noqa: E402
from _tempo6_final_sim import spectral                      # noqa: E402

BS = list(range(2, 9))          # 31.62 Hz .. 125.89 Hz
TRUTHS = [90, 100, 110, 120, 128, 140, 150, 160, 174]


# --------------------------------------------------------------------------
# The peak picker
# --------------------------------------------------------------------------
def pick(odf, moi_frames=5, rel_thr=0.30):
    """A simple peak picker: a local maximum that is the largest within
    +/- moi/2 frames, and above rel_thr of the window's 99.5th percentile.
    Returns a mask."""
    n = len(odf)
    thr = rel_thr * float(np.percentile(odf, 99.5))
    m = np.zeros(n, dtype=bool)
    half = moi_frames // 2
    for i in range(1, n - 1):
        if odf[i] < odf[i - 1] or odf[i] < odf[i + 1]:
            continue
        if odf[i] < thr:
            continue
        lo = max(0, i - half)
        hi = min(n, i + half + 1)
        if odf[i] >= odf[lo:hi].max():
            m[i] = True
    return m


def kick_frames(bpm, style, seconds=16.0):
    beat = 60.0 / bpm
    nb = int(seconds / beat)
    out = []
    for b in range(nb):
        t = b * beat
        ib = b % 4
        if style in ("house", "hardstyle"):
            out.append(t)
        elif style == "trap":
            if b % 2 == 0:
                out.append(t)
        elif style in ("dnb", "hiphop"):
            if ib in (0, 2):
                out.append(t)
        elif style == "garage":
            if ib in (0, 1):
                out.append(t)
    return np.array(out) * RATE


def comb_from(odf, P, win, mask=None, nbins=16):
    v = odf[-win:].astype(np.float64)
    idx = np.arange(len(v))
    w = v if mask is None else np.where(mask[-win:], v, 0.0)
    if w.sum() <= 0:
        return None
    ph = (idx % P) / P
    b = (ph * nbins).astype(int) % nbins
    hist = np.bincount(b, weights=w, minlength=nbins)
    k = int(np.argmax(hist))
    if 0 < k < nbins - 1:
        a, bb, c = hist[k - 1], hist[k], hist[k + 1]
        den = a - 2 * bb + c
        if abs(den) > 1e-12:
            d = 0.5 * (a - c) / den
            if abs(d) < 1:
                k = k + d
    return (k / nbins) % 1.0


def err_ms(phase, P, kicks, centre):
    e = []
    for t in kicks:
        j = round((t - centre) / P - phase)
        e.append(abs(centre + (phase + j) * P - t) / RATE * 1000.0)
    return np.array(e)


def signed_err_beats(phase, P, kicks, centre):
    """The signed distance from the truth kick to the published grid, in
    beats, wrapped to +/- half a beat."""
    e = []
    for t in kicks:
        j = round((t - centre) / P - phase)
        d = (centre + (phase + j) * P - t) / P
        e.append((d + 0.5) % 1.0 - 0.5)
    return np.array(e)


# --------------------------------------------------------------------------
# Q1: does peak-picking help the spectral route?
# --------------------------------------------------------------------------
def q1():
    print("=== does peak-picking the ODF help the spectral route? ===")
    print("%-14s %6s %9s %9s %9s" % ("odf", "wrong", "maxerr", "rmserr", "margin"))
    variants = [
        ("raw", None),
        ("picked", "pick"),
        ("picked-x", "pickx"),
    ]
    for label, mode in variants:
        errs = []
        wrong = 0
        margins = []
        for style in STYLES:
            for bpm in TRUTHS:
                odf = odf_of(BS, style, bpm)
                if mode == "pick":
                    m = pick(odf)
                    o = np.where(m, odf, 0.0)
                elif mode == "pickx":
                    # the peak's VALUE only, the rest of the envelope gone
                    m = pick(odf)
                    o = np.where(m, odf, 0.0)
                else:
                    o = odf
                e, marg, edge = spectral(o)
                err = e - bpm
                errs.append(err)
                margins.append(marg)
                if abs(err) >= 2.0:
                    wrong += 1
        errs = np.array(errs)
        margins = np.array(margins)
        print("%-14s %6d %9.2f %9.2f %9.2f" %
              (label, wrong, np.max(np.abs(errs)), np.sqrt(np.mean(errs ** 2)),
               np.mean(margins)))
    print()


# --------------------------------------------------------------------------
# Q2: the comb's bin count, on the ESTIMATED tempo, with the signed bias
# --------------------------------------------------------------------------
def q2():
    print("=== the phase comb: bin count, on the ESTIMATED tempo ===")
    print("%-6s %10s %10s %10s %12s %10s" %
          ("bins", "mean ms", "median ms", "max ms", "bias (beat)", "sd (beat)"))
    for nbins in (8, 12, 16, 20, 24, 32, 48, 64):
        errs = []
        biases = []
        for style in STYLES:
            for bpm in (100, 128, 140, 150, 160):
                odf = odf_of(BS, style, bpm)
                est, _, _ = spectral(odf)
                P = 60.0 / est * RATE
                kicks = kick_frames(bpm, style)
                W = int(4.0 * RATE)
                centre = len(odf) - W
                p = comb_from(odf, P, W, nbins=nbins)
                if p is None:
                    continue
                errs.append(np.mean(err_ms(p, P, kicks, centre)))
                biases.append(np.mean(signed_err_beats(p, P, kicks, centre)))
        errs = np.array(errs)
        biases = np.array(biases)
        print("%-6d %10.1f %10.1f %10.1f %12.4f %10.4f" %
              (nbins, np.mean(errs), np.median(errs), np.max(errs),
               np.mean(biases), np.std(biases)))
    print()

    print("=== the same, with the ODF peak-picked first (16 bins) ===")
    for label, dopick in (("raw", False), ("picked", True)):
        errs = []
        biases = []
        for style in STYLES:
            for bpm in (100, 128, 140, 150, 160):
                odf = odf_of(BS, style, bpm)
                if dopick:
                    odf = np.where(pick(odf), odf, 0.0)
                est, _, _ = spectral(odf)
                P = 60.0 / est * RATE
                kicks = kick_frames(bpm, style)
                W = int(4.0 * RATE)
                centre = len(odf) - W
                p = comb_from(odf, P, W, nbins=16)
                if p is None:
                    continue
                errs.append(np.mean(err_ms(p, P, kicks, centre)))
                biases.append(np.mean(signed_err_beats(p, P, kicks, centre)))
        errs = np.array(errs)
        biases = np.array(biases)
        print("  %-8s mean %6.1f ms  median %6.1f ms  max %6.1f ms  "
              "bias %+.4f beat  sd %.4f" %
              (label, np.mean(errs), np.median(errs), np.max(errs),
               np.mean(biases), np.std(biases)))
    print()


if __name__ == "__main__":
    q1()
    q2()
    sys.exit(0)

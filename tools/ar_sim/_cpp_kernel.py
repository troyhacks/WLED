#!/usr/bin/env python3
"""A LINE-BY-LINE TRANSLITERATION of ar_tempo.h's comb.

The C++ cannot be run on this machine - there is no host C++ compiler, and
the ESP32-P4's riscv toolchain produces binaries this PC cannot execute.
So the shipped source is transliterated here, statement by statement, and
the result is diffed against the vectorised port in _fw_port.py.  If the two
agree on random histograms at random rates, the C++ and the port are
computing the same box.

    python tools/ar_sim/_cpp_kernel.py

FLOAT32, NOT FLOAT64.

This used to be a float64 transliteration, and it proved nothing about the
shipped code: the P4 has a single-precision FPU and the C++ is written in
float, so the arithmetic that decides which bin an onset lands in is done in
single precision.  An onset that sits within a thousandth of a bin edge
lands in a DIFFERENT bin on the board than it does in double precision, and
one onset moved between neighbouring bins moves the score by up to 1.7 % -
which is a tenth of the veto's margin.  So the transliteration is float32
here, statement for statement, and the port's PHASE is float32 too.

What is left of the difference between the two is the BOX: the firmware
accumulates the histogram and slides the window in float32, one step at a
time, and the port uses a float64 cumulative sum.  That is a hundredth of
the bin-flip effect and it is the difference the port accepts.
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _fw_port as P                                  # noqa: E402

BINS = P.COMB_BINS
f32 = np.float32


def cpp_kernel_half_width(period, tol_ms):
    """arTempoKernelHalfWidth(period, &wholeOut, &fracOut)."""
    tol_frames = f32(f32(tol_ms * 0.001) * f32(P.RATE))
    wf = f32(f32(tol_frames / period) * f32(BINS))
    wmax = f32(BINS // 2 - 2)
    if wf < f32(1.0):
        wf = f32(1.0)
    if wf > wmax:
        wf = wmax
    w = int(wf)
    return w, f32(wf - f32(w))


def cpp_score(frames, strengths, bpm, tol_ms=P.TOL_MS):
    """arTempoCombScore(), statement for statement, in float32."""
    n = len(frames)
    total = f32(0.0)
    for i in range(n):
        total = f32(total + strengths[i])

    period = f32(f32(f32(60.0) * f32(P.RATE)) / f32(bpm))
    inv_period = f32(f32(1.0) / period)
    bin_scale = f32(f32(BINS) * inv_period)

    # the histogram
    h = np.zeros(BINS, dtype=f32)
    for i in range(n):
        f = f32(frames[i])
        # the C++'s phase: a FLOOR of the quotient, and the two
        # corrections for a multiply that rounded across an integer
        k = f32(np.floor(f32(f * inv_period)))
        u = f32(f - f32(k * period))
        if u < f32(0.0):
            u = f32(u + period)
        elif u >= period:
            u = f32(u - period)
        b = int(f32(u * bin_scale))
        if b < 0:
            b = 0
        elif b >= BINS:
            b = BINS - 1
        h[b] = f32(h[b] + strengths[i])

    if total <= f32(0.0):
        return f32(0.0), f32(0.0)

    w, frac = cpp_kernel_half_width(period, tol_ms)

    # the rolling sum, slid one bin at a time
    win = f32(0.0)
    for j in range(-w, w + 1):
        win = f32(win + h[(j + BINS) % BINS])
    best = f32(-1.0e30)
    best_bin = 0
    for i in range(BINS):
        if i > 0:
            win = f32(win + f32(h[(i + w) % BINS]
                                - h[(i - w - 1 + BINS) % BINS]))
        v = win
        if frac < f32(0.5):
            v = f32(v - f32(f32(f32(0.5) - frac)
                             * f32(h[(i - w + BINS) % BINS]
                                   + h[(i + w) % BINS])))
        else:
            v = f32(v + f32(f32(frac - f32(0.5))
                             * f32(h[(i - w - 1 + BINS) % BINS]
                                   + h[(i + w + 1) % BINS])))
        if v > best:
            best = v
            best_bin = i
    return f32(best / total), f32(f32(f32(best_bin) / f32(BINS)) * period)


def cpp_scores(frames, strengths, bpms, tol_ms=P.TOL_MS):
    """The C++'s score at many candidate rates at once."""
    fr = np.asarray(frames, dtype=f32)
    st = np.asarray(strengths, dtype=f32)
    out = np.zeros(len(bpms))
    ph = np.zeros(len(bpms))
    for i, bpm in enumerate(bpms):
        s, p = cpp_score(fr, st, bpm, tol_ms)
        out[i] = float(s)
        ph[i] = float(p)
    return out, ph


def main():
    rng = np.random.default_rng(20241103)
    worst = 0.0
    worst_rel = 0.0
    for trial in range(8):
        n = int(rng.integers(30, 300))
        frames = np.sort(rng.uniform(0, 40, n))
        strengths = rng.uniform(0.01, 1.0, n)
        bpms = np.array(sorted(rng.uniform(90.0, 175.0, 12)))
        a, _ = cpp_scores(frames, strengths, bpms)
        tr = P.Tracker()
        tr.exact_kernel = True
        b, _ = tr.comb_scores_all(frames, strengths, bpms)
        d = np.abs(a - b)
        rel = d / np.maximum(np.abs(b), 1e-12)
        worst = max(worst, float(d.max()))
        worst_rel = max(worst_rel, float(rel.max()))
        print("trial %2d: n=%3d  max abs %.3e  max rel %.3e"
              % (trial, n, d.max(), rel.max()))
    print()
    print("the C++ transliterated in float32 vs the port:")
    print("  worst absolute difference  %.3e" % worst)
    print("  worst relative difference  %.3e" % worst_rel)
    print("  the port's BOX is a float64 cumulative sum and the")
    print("  firmware's is a float32 rolling sum, so the two are")
    print("  not bit-identical - a few 1e-7 is expected.")
    ok = worst_rel < 1e-4
    print("  VERDICT: %s" % ("MATCH" if ok else "MISMATCH"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

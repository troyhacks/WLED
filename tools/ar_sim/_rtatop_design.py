"""Design the RTA's top-23 kernel: the ENBW is the target, not the -3 dB width.

This is the offline twin of the firmware's design search - the same arithmetic
in double precision, so the board's numbers can be checked against it.

The question: the RTA's bottom 8 bands (19.95-100 Hz) come from a 4th-order
Butterworth bandpass bank running at 3 kHz.  The top 23 (125.9 Hz - 19 952.6 Hz)
used to come from the FFT's 4096-sample window, whose content cannot be newer
than 42.7 ms.  Moving them onto a bank at the full rate needs a kernel that

  * has the SAME ENBW convention as the bottom 8 (0.85 x the nominal width),
    or the RTA view gets a step at band 8,
  * has peak gain exactly 1.0 at the band centre, so a centred tone reads its
    own power,
  * does not leak into the neighbouring bands more than the bottom 8 does.

Targeting the ENBW needs a search, because the bilinear transform's warping
moves it about: the plain design's ENBW/width ratio drifts from 0.85 at 126 Hz
down to 0.164 at 19 952 Hz.  The firmware does the search with the Parseval
identity - ENBW = fs/2 * sum(h^2) - which needs no integration and no
statistics.  This file is that search, in double precision.

Measured on the board (2026-10-26), from /json/ar -> rtatopbank:
    ENBW vs the impulse response   worst  0.0005 dB
    tone gain at the centre        worst -0.0217 dB
    neighbour leakage              -12.3 dB at the bottom, -7.8 dB at the top
    ENBW ratio (dB)                -0.719 .. -0.705   (the bottom 8: -0.740 .. -0.706)
"""

import math

FS = 48000.0
ISO_DOWN = 0.890898718
ISO_UP = 1.122462048
TARGET_RATIO = 0.85          # the bottom bank's ENBW / nominal width
N_BOTTOM = 8                 # the bands the bottom bank already owns


def iso_bands(n=31):
    return [(10.0 ** ((13 + k) * 0.1),
             10.0 ** ((13 + k) * 0.1) * ISO_DOWN,
             10.0 ** ((13 + k) * 0.1) * ISO_UP) for k in range(n)]


def rbj_bpf(f0, fs, Q):
    """The firmware's bankRbjBpf: b0,b1,b2,a1,a2, peak gain 1 at f0."""
    w0 = 2.0 * math.pi * f0 / fs
    alpha = math.sin(w0) / (2.0 * Q)
    a0 = 1.0 + alpha
    return (alpha / a0, 0.0, -alpha / a0,
            -2.0 * math.cos(w0) / a0, (1.0 - alpha) / a0)


def sec_mag2(c, f, fs):
    w = 2.0 * math.pi * f / fs
    cw, sw = math.cos(w), math.sin(w)
    c2 = cw * cw - sw * sw
    s2 = 2.0 * cw * sw
    nr = c[0] + c[1] * cw + c[2] * c2
    ni = c[1] * (-sw) + c[2] * (-s2)
    dr = 1.0 + c[3] * cw + c[4] * c2
    di = c[3] * (-sw) + c[4] * (-s2)
    return (nr * nr + ni * ni) / (dr * dr + di * di)


def cas_mag2(c1, c2, f, fs):
    return sec_mag2(c1, f, fs) * sec_mag2(c2, f, fs)


def enbw_impulse(c1, c2, fs, n=4096):
    """Parseval: the one-sided ENBW is fs/2 times the sum of h^2.

    The impulse goes through the SAME recursion the bank runs, so this
    measures the code and not a model of it.
    """
    s1a = s2a = s1b = s2b = 0.0
    h2 = 0.0
    for i in range(n):
        x = 1.0 if i == 0 else 0.0
        y = c1[0] * x + s1a
        s1a = c1[1] * x - c1[3] * y + s2a
        s2a = c1[2] * x - c1[4] * y
        z = c2[0] * y + s1b
        s1b = c2[1] * y - c2[3] * z + s2b
        s2b = c2[2] * y - c2[4] * z
        h2 += z * z
    return 0.5 * fs * h2


def enbw_numeric(c1, c2, fs, n=1200):
    """The other way round: the integral of |H|^2 over the spectrum."""
    tot = 0.0
    f0, f1 = 0.01, fs * 0.5
    fprev = f0
    prev = cas_mag2(c1, c2, f0, fs)
    for i in range(1, n + 1):
        f = f0 * (fs * 0.5 / f0) ** (i / n)
        cur = cas_mag2(c1, c2, f, fs)
        tot += 0.5 * (prev + cur) * (f - fprev)
        prev, fprev = cur, f
    return tot


def half_power_width(c1, c2, fs, fc):
    """The digital -3 dB width, found by walking out from the peak."""
    peak = cas_mag2(c1, c2, fc, fs)
    step = fc * 0.0005
    lo, hi = 0.0, fs * 0.5
    f = fc
    while f > 0.0:
        f -= step
        if f <= 0.0 or cas_mag2(c1, c2, f, fs) <= 0.5 * peak:
            lo = f
            break
    f = fc
    while f < fs * 0.5:
        f += step
        if f >= fs * 0.5 or cas_mag2(c1, c2, f, fs) <= 0.5 * peak:
            hi = f
            break
    return hi - lo


# The 4th-order Butterworth bandpass's two pole Qs: 1/(2 sin(pi/8)) and
# 1/(2 sin(3 pi/8)).  The bottom bank uses the same two.
POLE_Q = (1.0 / (2.0 * math.sin(math.pi / 8.0)),
          1.0 / (2.0 * math.sin(3.0 * math.pi / 8.0)))


def design(fc, width, fs, scale):
    """The bottom bank's design, with the scale as the width knob."""
    ratio = fc / width
    q1 = scale * POLE_Q[0] * ratio
    q2 = scale * POLE_Q[1] * ratio
    return rbj_bpf(fc, fs, q1), rbj_bpf(fc, fs, q2)


def solve_scale(fc, width, fs, target_enbw, tol_db=0.02, iters=14):
    """The scale that puts the ENBW on the target.

    The ENBW falls monotonically as the scale rises, so the search is a
    bisection on the geometric mean of the bracket.  The plain design is tried
    first and accepted when it is already on the target - which is the case for
    every band below 1.6 kHz, and that is most of the work saved.
    """
    c1, c2 = design(fc, width, fs, 1.0)
    e = enbw_impulse(c1, c2, fs)
    if abs(10.0 * math.log10(e / target_enbw)) < tol_db:
        return 1.0, 0, e
    lo, hi = 0.02, 50.0
    for _ in range(iters):
        mid = math.sqrt(lo * hi)
        c1, c2 = design(fc, width, fs, mid)
        if enbw_impulse(c1, c2, fs) > target_enbw:
            lo = mid
        else:
            hi = mid
    mid = math.sqrt(lo * hi)
    c1, c2 = design(fc, width, fs, mid)
    return mid, iters, enbw_impulse(c1, c2, fs)


def main():
    bands = iso_bands()
    top = list(range(N_BOTTOM, 31))

    # Design every band, and keep the two sections so the leakage can be
    # measured the way the firmware measures it.
    des = {}
    for k in top:
        fc, lo, hi = bands[k]
        width = hi - lo
        scale, iters, e = solve_scale(fc, width, FS, TARGET_RATIO * width)
        des[k] = (scale, iters, e, design(fc, width, FS, scale))

    # The leakage matrix: M[j][k] is band j's filter's power response at band
    # k's centre.  The firmware's self-test puts a tone at band k's centre and
    # reads band j - so its "neighbour" number is M[j][k], the transpose of the
    # obvious one.  Getting this the right way round matters: the two differ
    # by up to 2 dB in the top bands.
    M = {}
    for j in top:
        c1, c2 = des[j][3]
        for k in top:
            M[(j, k)] = 10 * math.log10(cas_mag2(c1, c2, bands[k][0], FS))

    print("the bottom 8, as the board has them (3 kHz): ENBW/width 0.8498 -> 0.8433")
    print()
    print("the top 23 at 48 kHz, ENBW targeted at 0.85 x the nominal width:")
    print("   k     fc      width    ENBW    ratio      -3dB     gain    nbr lo   nbr hi    scale")
    worst_nbr = -99.0
    worst_ratio = 0.0
    worst_enbw_disagree = 0.0
    for k in top:
        fc, lo, hi = bands[k]
        width = hi - lo
        scale, iters, e, (c1, c2) = des[k]
        w = half_power_width(c1, c2, FS, fc)
        gain = 10 * math.log10(cas_mag2(c1, c2, fc, FS))
        # The neighbours' filters, read at THIS band's centre.
        nb = []
        for j in (k - 1, k + 1):
            if j in des:
                nb.append(M[(j, k)])
        worst_nbr = max(worst_nbr, max(nb))
        rd = 10 * math.log10(e / width)
        worst_ratio = max(worst_ratio, abs(rd - 10 * math.log10(TARGET_RATIO)))
        # Parseval against the integral: the firmware's self-test check.
        disagree = 10 * math.log10(e / enbw_numeric(c1, c2, FS))
        worst_enbw_disagree = max(worst_enbw_disagree, abs(disagree))
        nbs = ("%+7.2f  %+7.2f" % (nb[0], nb[1])) if len(nb) == 2 else ("%+7.2f  %+7s" % (nb[0], "--"))
        print("  %2d %9.3f %8.3f %8.3f  %.4f %+6.3f dB %8.3f %+7.4f  %s   %.4f (%d iters)"
              % (k, fc, width, e, e / width, rd, w, gain, nbs, scale, iters))

    print()
    print("  worst neighbour leakage:        %+.2f dB" % worst_nbr)
    print("  worst ENBW-ratio error vs 0.85:  %.4f dB" % worst_ratio)
    print("  worst Parseval vs integral:      %.6f dB" % worst_enbw_disagree)

    # The board's numbers, to check against.
    board = [-12.288, -12.336, -12.336, -12.360, -12.316, -12.348, -12.359, -12.361,
             -12.358, -12.327, -12.327, -12.296, -12.280, -12.248, -12.188, -12.098,
             -11.948, -11.731, -11.367, -10.783, -9.772, -7.836, -17.597]
    print()
    print("  band   firmware (dB)   this file (dB)    diff")
    worst_diff = 0.0
    for i, k in enumerate(top):
        mine = max(M[(j, k)] for j in (k - 1, k + 1) if j in des)
        d = mine - board[i]
        worst_diff = max(worst_diff, abs(d))
        print("   %2d %9.3f %12.3f %8.3f" % (k, board[i], mine, d))
    print("  worst difference: %.3f dB" % worst_diff)


if __name__ == "__main__":
    main()

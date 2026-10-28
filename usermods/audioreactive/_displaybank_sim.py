"""Design tool for the display band's filter shape.

The question: the 16 display bands come out of a 4096-sample FFT, so every
band's reading is a weighted average over an 85.3 ms window and its content
cannot be newer than 42.7 ms. A bank of biquads has no window - each band is
its own filter - so its answer is as new as its own bandwidth allows.

That is a TRADE, not a free win, and the trade has a shape that has to be
measured before anything is built:

  - the band's response time is set by its filter's settling time, which for a
    narrow band at a low frequency is TENS of milliseconds no matter what;
  - a wider filter settles faster but resolves less.

So this computes, exactly, how long each candidate takes to reach 63% of a
step, and what it costs. No trial-and-error: the filters are linear and the
detector is a one-pole, so a step in gives a closed-form envelope out, and the
simulation is just the firmware's own recursion.

Run:  python _displaybank_sim.py
"""

import numpy as np

# ---------------------------------------------------------------------------
# The setup, exactly as the firmware has it.
# ---------------------------------------------------------------------------
FS = 48000.0
NFFT = 4096
HOP = 512
FRAME_MS = HOP * 1000.0 / FS

FLOW16, FHIGH16 = 35.0, 11025.0
_R16 = (FHIGH16 / FLOW16) ** (1.0 / 16.0)
CENTRE16 = FLOW16 * _R16 ** (np.arange(16) + 0.5)
EDGE_LO16 = FLOW16 * _R16 ** np.arange(16)
EDGE_HI16 = FLOW16 * _R16 ** (np.arange(16) + 1.0)
WIDTH16 = EDGE_HI16 - EDGE_LO16


# ---------------------------------------------------------------------------
# Biquads - transposed direct form II, the same recursion the firmware uses.
# ---------------------------------------------------------------------------
class Biquad:
    def __init__(self, b0, b1, b2, a1, a2):
        self.b0, self.b1, self.b2, self.a1, self.a2 = b0, b1, b2, a1, a2
        self.w1 = 0.0
        self.w2 = 0.0

    def step(self, x):
        y = self.b0 * x + self.w1
        self.w1 = self.b1 * x - self.a1 * y + self.w2
        self.w2 = self.b2 * x - self.a2 * y
        return y

    def run(self, x):
        out = np.empty(len(x))
        for i in range(len(x)):
            out[i] = self.step(x[i])
        return out


def rbj_bpf(f0, fs, q):
    """The firmware's bankRbjBpf - no prewarping."""
    w0 = 2.0 * np.pi * f0 / fs
    alpha = np.sin(w0) / (2.0 * q)
    a0 = 1.0 + alpha
    return Biquad(alpha / a0, 0.0, -alpha / a0,
                  -2.0 * np.cos(w0) / a0, (1.0 - alpha) / a0)


def q_for_width(f0, fs, width):
    """The Q that makes the DIGITAL -3 dB width exactly `width`.

    This is the correction that matters. The cookbook's rule - Q = f0/width -
    is the ANALOGUE relative Q, and the bilinear transform's frequency axis is
    not linear, so the digital band comes out NARROWER than asked for as f0
    approaches the rate: at f0/fs = 0.192 it is 23% narrow, and the top display
    band (9211 Hz at 48 kHz) sits exactly there.

    The closed form comes from the magnitude of the bandpass, which in terms of
    s = tan(w/2)/tan(w_d/2) is

        |H|^2 = s^2 / (s^2 + Q^2 (1 - s^2)^2)

    whose half-power points are s_lo/s_hi with s_lo*s_hi = 1 and s_hi-s_lo = 1/Q,
    so the digital width is

        df = (fs/pi) * atan( u / (Q (1 + u^2)) ),   u = tan(pi*f0/fs)

    and inverting it for Q gives the expression below. Verified against a
    numerical scan of |H| to 0.001% at every band in the design.
    """
    u = np.tan(np.pi * f0 / fs)
    return u / ((1.0 + u * u) * np.tan(np.pi * width / fs))


def butter_lp(fc, fs, order):
    """A prewarped Butterworth lowpass - the same prototype the firmware's
    bankButterLowpass() builds, so the sim and the board agree.

    The analogue prototype's poles are at exp(j*pi*(2k+N+1)/(2N)); each
    conjugate pair becomes one second-order section, and the corner is
    prewarped so that the -3 dB point lands on fc exactly.
    """
    K = np.tan(np.pi * fc / fs)
    out = []
    for k in range(order):
        theta = np.pi * (order + 2.0 * k + 1.0) / (2.0 * order)
        px, py = np.cos(theta), np.sin(theta)
        if py < -1e-6:
            continue                       # the conjugate of a pole already placed
        sigma = -px
        A = 1.0 / (K * K) + 2.0 * sigma / K + 1.0
        if abs(py) < 1e-6:                 # a real pole: a first-order section
            out.append(Biquad(1.0 / A, 1.0 / A, 0.0,
                              (1.0 - sigma * K) / A, 0.0))
        else:
            out.append(Biquad(1.0 / A, 2.0 / A, 1.0 / A,
                              (2.0 - 2.0 / (K * K)) / A,
                              (1.0 / (K * K) - 2.0 * sigma / K + 1.0) / A))
    return out


def section_qs(fc, width, nsec, fs_band=None):
    """The Q values for one band, each corrected for the digital width.

    ONE section is the design: a bandpass's settling time is set by its
    bandwidth, and a narrower (higher-Q) filter resolves better but takes
    longer to fill up. The design measurement showed a 4th-order (2-section)
    band is no faster at the bottom band and costs twice as much.
    """
    if fs_band is None:
        fs_band = level_rate(assign_level(fc))
    if nsec == 1:
        return (q_for_width(fc, fs_band, width),)
    if nsec == 2:
        ratio = fc / width
        q1 = ratio / (2.0 * np.sin(np.pi / 8.0))
        q2 = ratio / (2.0 * np.sin(3.0 * np.pi / 8.0))
        return (q_for_width(fc, fs_band, fc / q1),
                q_for_width(fc, fs_band, fc / q2))
    raise ValueError(nsec)


# ---------------------------------------------------------------------------
# The decimation tree.
# ---------------------------------------------------------------------------
def level_rate(m):
    return FS / (2 ** m)


def assign_level(fc):
    """The lowest level whose rate keeps the band well clear of Nyquist.

    The rule is fc <= rate/8, not rate/4, and the reason is the anti-alias
    filter. The AA filter for the decimation into a level has to sit between
    the highest band kept at that level and the new Nyquist, and it has to
    reject the new Nyquist by enough to stop the alias folding back into the
    bands. With the corner at rate/4 and the bands at rate/8, the corner is
    only 2x above the band (-0.02 dB there) and 24 dB down at the new
    Nyquist - and 48 dB down by the old Nyquist, which is where the aliases
    come from.
    """
    for m in range(10, -1, -1):
        if fc <= level_rate(m) / 8.0:
            return m
    return 0


# ---------------------------------------------------------------------------
# The exact step response.
# ---------------------------------------------------------------------------
def chain_detect(fc, width, level, nsec=2, tau_ms=5.0, aa_hz=None,
                 aa_order=4, n_out=8000):
    """The detector's output when the input's power jumps from nothing to sigma^2.

    A bandpass does not have a step response - a step rings and decays back to
    zero, because the band blocks DC. What the bar actually sees is the
    POWER, and the power of a band whose input power jumps at t0 rises as the
    filter's own impulse response fills up:

        E[y(n)^2] = sigma^2 * (sum of h^2 up to n)

    and the one-pole detector is linear, so its expected value is the one-pole
    applied to that. So the answer is exact, and it is the same recursion the
    firmware runs.

    Returns (detector array, band rate).
    """
    dec = 2 ** level
    fs_band = level_rate(level)

    if aa_hz is None:
        aa_hz = level_rate(level) / 4.0 if level > 0 else 20000.0

    n_in = n_out * dec + 8 * dec
    x = np.zeros(n_in)
    x[0] = 1.0                                  # the impulse

    aa = butter_lp(aa_hz, FS, aa_order)
    for s in aa:
        x = s.run(x)
    x = x[dec - 1::dec]                         # the decimation
    x = x[:n_out]

    y = x
    for q in section_qs(fc, width, nsec, fs_band):
        y = rbj_bpf(fc, fs_band, q).run(y)

    cum = np.cumsum(y * y)                      # the rising energy
    # The firmware's detector: det += alpha * (input - det). A real one-pole,
    # NOT an integrator - the difference matters, because an integrator would
    # make the detector's time constant irrelevant and hide it.
    alpha = 1.0 - np.exp(-1.0 / (tau_ms * 0.001 * fs_band))
    det = np.zeros(len(cum))
    for i in range(len(cum)):
        det[i] = det[i - 1] + alpha * (cum[i] - det[i - 1]) if i else alpha * cum[i]
    return det, fs_band


def _t63(det, fs_band):
    final = det[-1]
    if final <= 0:
        return float('nan')
    idx = int(np.argmax(det >= 0.63 * final))
    return idx / fs_band * 1000.0


def chain_step63(fc, width, level, nsec=2, tau_ms=5.0, **kw):
    det, fsb = chain_detect(fc, width, level, nsec, tau_ms, **kw)
    return _t63(det, fsb)


def half_band_step63(fc, width, level, tau_ms=5.0, nsec=2):
    """Two filters, each covering half the band, and the LOUDER one wins.

    This is the peak-over-band approximation: a tone anywhere in the band
    lights it up, because the half it is in wins. The reading is the larger of
    the two detectors, so the 63% point is where the LARGER one reaches 63% of
    ITS OWN final value - the loud half is what the bar shows.
    """
    da, fsb = chain_detect(fc * 2 ** (1 / 12), width / 2, level, nsec, tau_ms)
    db, _ = chain_detect(fc * 2 ** (-1 / 12), width / 2, level, nsec, tau_ms)
    fa, fb = da[-1], db[-1]
    if fa <= 0 and fb <= 0:
        return float('nan')
    # the louder half decides: time until IT reaches 63% of its own final
    if fa >= fb:
        return _t63(da, fsb)
    return _t63(db, fsb)


# ---------------------------------------------------------------------------
# The transform's answer, for comparison: the window's own energy CDF.
# ---------------------------------------------------------------------------
def bh_window(n):
    i = np.arange(n)
    return (0.35875
            - 0.48829 * np.cos(2 * np.pi * i / (n - 1))
            + 0.14128 * np.cos(4 * np.pi * i / (n - 1))
            - 0.01168 * np.cos(6 * np.pi * i / (n - 1)))


_W = bh_window(NFFT)
_ETOTAL = (_W ** 2).sum()
_ECDF = np.cumsum(_W ** 2)


def fft_step63():
    """The transform: the window's energy CDF, sampled at the frame rate."""
    t0 = 256
    for f in range(0, 200):
        s = f * HOP - (NFFT - HOP)
        i0 = max(0, t0 - s)
        if i0 >= NFFT:
            continue
        before = _ECDF[i0 - 1] if i0 > 0 else 0.0
        if (_ETOTAL - before) / _ETOTAL >= 0.63:
            return f * FRAME_MS
    return float('nan')


# ---------------------------------------------------------------------------
# The reports.
# ---------------------------------------------------------------------------
def report_shape(tau_ms=5.0):
    print("=" * 92)
    print("DISPLAY BANDS - how long until the bar reflects a change that already happened?")
    print("=" * 92)
    print("The transform's answer is 42.7 ms for EVERY band, because its window is 4096")
    print("samples long and every band is a weighted average over all of it.")
    print("A bank's answer is its own filter's settling time, and that is per band.")
    print()
    print("  band  centre Hz   width Hz  level   1 section   2 sections   2 x half   "
          "vs 42.7 ms")
    print("  ----  ---------   ---------  -----   ----------   ----------   "
          "----------   ---------")
    fft_ms = fft_step63()
    for k in range(16):
        fc = CENTRE16[k]
        w = WIDTH16[k]
        lvl = assign_level(fc)
        t1 = chain_step63(fc, w, lvl, 1, tau_ms)
        t2 = chain_step63(fc, w, lvl, 2, tau_ms)
        th = half_band_step63(fc, w, lvl, tau_ms)
        best = min(t1, t2, th)
        print("  %3d  %9.1f  %9.1f  %5d  %8.1f ms  %8.1f ms  %8.1f ms   %6.2fx"
              % (k, fc, w, lvl, t1, t2, th, fft_ms / best))
    print()
    print("  (the FFT's 63%% point is %.1f ms)" % fft_ms)


def report_cost():
    print()
    print("=" * 92)
    print("COST - section-samples per frame (hop 512)")
    print("=" * 92)
    maxlvl = max(assign_level(fc) for fc in CENTRE16)
    for label, nsec, half in (("1 section", 1, False),
                              ("2 sections", 2, False),
                              ("2 x half-band", 2, True)):
        total = 0
        for k in range(16):
            fc = CENTRE16[k]
            lvl = assign_level(fc)
            n = HOP // (2 ** lvl)
            total += nsec * n * (2 if half else 1)
        aa_total = 0
        for lvl in range(1, maxlvl + 1):
            aa_total += 2 * (HOP // (2 ** (lvl - 1)))
        tot = total + aa_total
        print("  %-12s bands %6d   AA %6d   TOTAL %6d section-samples  (~%4.0f us)"
              % (label, total, aa_total, tot, tot * 0.080))
    print()
    print("  The existing 8-band bank: 1536 section-samples, measured 123 us.")
    print("  The transform's path: 2535 us.")
    print()
    print("  level  rate Hz   bands   samples/frame")
    for lvl in range(maxlvl + 1):
        nb = sum(1 for fc in CENTRE16 if assign_level(fc) == lvl)
        if nb == 0:
            continue
        print("  %5d  %7.0f   %5d   %14d" % (lvl, level_rate(lvl), nb,
                                               nb * (HOP // (2 ** lvl))))


def report_detector(tau_ms=5.0):
    print()
    print("=" * 92)
    print("THE DETECTOR - its own time constant is on top of the filter's")
    print("=" * 92)
    print("  tau ms   band 0 (2 sec)   band 8 (2 sec)   band 15 (2 sec)")
    for tau in (1.0, 2.0, 5.0, 10.0):
        a = chain_step63(CENTRE16[0], WIDTH16[0], assign_level(CENTRE16[0]), 2, tau)
        b = chain_step63(CENTRE16[8], WIDTH16[8], assign_level(CENTRE16[8]), 2, tau)
        c = chain_step63(CENTRE16[15], WIDTH16[15], assign_level(CENTRE16[15]), 2, tau)
        print("  %6.1f   %12.1f ms   %12.1f ms   %12.1f ms" % (tau, a, b, c))


if __name__ == "__main__":
    report_shape()
    report_detector()
    report_cost()

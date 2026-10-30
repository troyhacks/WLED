"""Settles one question: can a bank of biquads measure the 31 ISO 266 bands
faster - and more accurately - than the transform does?

The transform's estimate is a weighted average over its whole 4096-sample
window, so its answer to "what changed?" cannot be newer than 85.3 ms, no
matter how often the frame is published. A filter bank has no window: each
band's answer is that band's own filter, and its age is set by the band's own
bandwidth. So the two are not interchangeable and the comparison has to be
made on RESPONSE TIME, not on cost.

Both responses are computed EXACTLY rather than by trial, because both paths
are linear in the input's second moment:

  FFT path   the estimate is a quadratic form in the windowed samples, so for
             a signal that switches on at T0 its expected value rises as the
             cumulative ENERGY of the window from T0 onwards. The normalised
             response is the window's own energy CDF.

  bank path  the band filter is an LTI system with impulse response h, so
             E[y(n)^2] = sigma^2 * (sum of h^2 over the samples since T0),
             and the one-pole detector is linear in y^2, so its expected value
             follows exactly.

No Monte Carlo, no sampling noise, and the same arithmetic the firmware does.

Run:  python _bankvsfft_sim.py
"""

import numpy as np

# ---------------------------------------------------------------------------
# The setup, exactly as the firmware has it.
# ---------------------------------------------------------------------------

FS = 48000.0
NFFT = 4096
HOP = 512
FRAME_MS = HOP * 1000.0 / FS

ISO_EDGE_DOWN = 0.890898718
ISO_EDGE_UP = 1.122462048


def bh_window(n):
    """ESP-DSP's dsps_wind_blackman_harris_f32."""
    i = np.arange(n)
    return (0.35875
            - 0.48829 * np.cos(2 * np.pi * i / (n - 1))
            + 0.14128 * np.cos(4 * np.pi * i / (n - 1))
            - 0.01168 * np.cos(6 * np.pi * i / (n - 1)))


CENTRE31 = np.array([10.0 ** (1.3 + 0.1 * k) for k in range(31)])
EDGE_LO31 = CENTRE31 * ISO_EDGE_DOWN
EDGE_HI31 = CENTRE31 * ISO_EDGE_UP

FLOW16, FHIGH16 = 35.0, 11025.0
_R16 = (FHIGH16 / FLOW16) ** (1.0 / 16.0)
CENTRE16 = FLOW16 * _R16 ** (np.arange(16) + 0.5)
EDGE_LO16 = FLOW16 * _R16 ** np.arange(16)
EDGE_HI16 = FLOW16 * _R16 ** (np.arange(16) + 1.0)


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


def butter_lp(fc, fs, sections):
    """A Butterworth lowpass as a cascade of RBJ sections."""
    out = []
    for s in range(sections):
        q = 0.7071067811865476 / (2.0 * np.sin(np.pi * (2 * s + 1) / (4.0 * sections)))
        w0 = 2.0 * np.pi * fc / fs
        alpha = np.sin(w0) / (2.0 * q)
        a0 = 1.0 + alpha
        out.append(Biquad((1 - np.cos(w0)) / 2 / a0, (1 - np.cos(w0)) / a0,
                          (1 - np.cos(w0)) / 2 / a0,
                          -2 * np.cos(w0) / a0, (1 - alpha) / a0))
    return out


def iso_section_qs(fc, width):
    """The firmware's band Qs: a 4th-order Butterworth bandpass."""
    ratio = fc / width
    return (ratio / (2.0 * np.sin(np.pi / 8.0)),
            ratio / (2.0 * np.sin(3.0 * np.pi / 8.0)))


# ---------------------------------------------------------------------------
# The dyadic decimation tree: which rate does each band run at?
# ---------------------------------------------------------------------------

MAX_LEVEL = 10


def level_rate(m):
    return FS / (2 ** m)


def assign_levels(centres):
    """A band joins the LOWEST level whose rate keeps it under fs/4, which is
    where a digital bandpass still behaves."""
    out = []
    for f in centres:
        m = 0
        while m + 1 <= MAX_LEVEL and f <= level_rate(m) / 4.0:
            m += 1
        out.append(m)
    return out


def chain_impulse_band_rate(fc, width, level, aa_hz=400.0, aa_secs=2, n_imp=8192):
    """The band's composite impulse response AT THE BAND RATE.

    48 kHz -> anti-alias -> decimate -> two bandpass sections.
    Returns h (length n_imp/dec) sampled at fs_band.
    """
    dec = 2 ** level
    fs_band = level_rate(level)

    imp = np.zeros(n_imp)
    imp[0] = 1.0
    for s in butter_lp(aa_hz, FS, aa_secs):
        imp = s.run(imp)
    imp = imp[::dec]

    q1, q2 = iso_section_qs(fc, width)
    for q in (q1, q2):
        imp = rbj_bpf(fc, fs_band, q).run(imp)
    return imp, fs_band, dec


def detector_response(h_band, tau_ms, fs_band):
    """d[n] for a signal that starts at band sample 0, normalised to d[inf].

    E[y(n)^2] = sigma^2 * cumsum(h^2)[n]; the detector is a one-pole, which is
    linear, so its expected value is the one-pole applied to that.
    """
    cum = np.cumsum(h_band ** 2)
    alpha = 1.0 - np.exp(-1.0 / (tau_ms * 0.001 * fs_band))
    d = np.zeros(len(cum))
    prev = 0.0
    acc = 0.0
    for n in range(len(cum)):
        acc += alpha * (cum[n] - prev)
        prev = cum[n]
        d[n] = acc
    return d / d[-1] if d[-1] > 0 else d


# ---------------------------------------------------------------------------
# The transform's response: the window's energy CDF.
# ---------------------------------------------------------------------------

_W = bh_window(NFFT)
_E = _W ** 2
_ETOTAL = _E.sum()
_ECDF = np.cumsum(_E)


def fft_response(t0, frame):
    """Normalised expected estimate at `frame` after a signal that starts at
    sample t0.

    The window at that frame covers [frame*HOP-3584, frame*HOP) and always
    holds NFFT samples. The samples before t0 carry no signal, so the
    estimate is the window's energy from t0 onwards, over the window's total.
    """
    s = frame * HOP - (NFFT - HOP)
    i0 = max(0, t0 - s)
    if i0 >= NFFT:
        return 0.0
    before = _ECDF[i0 - 1] if i0 > 0 else 0.0
    return (_ETOTAL - before) / _ETOTAL


def fft_response_at(t0, frame):
    return fft_response(t0, frame)


# ---------------------------------------------------------------------------
# Report.
# ---------------------------------------------------------------------------

def response_table():
    print("=" * 80)
    print("RESPONSE TIME - how long until a band reports a change that already happened?")
    print("=" * 80)
    print("Frame period %.3f ms -> frames are published at %.1f Hz."
          % (FRAME_MS, 1000.0 / FRAME_MS))
    print("The transform cannot answer with anything newer than its window (%.1f ms)."
          % (NFFT * 1000.0 / FS))
    print("A filter bank's answer is as new as its own band allows.\n")

    t0 = 256
    assign = assign_levels(CENTRE31)

    print("  band  centre Hz   width Hz   level   rate Hz   FFT 63%    bank 63%   speed-up")
    print("  ----  ---------   ---------  -----   -------   ---------   ---------  ---------")
    for k in range(31):
        fc = CENTRE31[k]
        width = EDGE_HI31[k] - EDGE_LO31[k]
        lvl = assign[k]

        fft_ms = None
        for f in range(0, 60):
            if fft_response(t0, f) >= 0.63:
                fft_ms = f * FRAME_MS
                break

        h, fs_band, dec = chain_impulse_band_rate(fc, width, lvl)
        d = detector_response(h, 5.0, fs_band)
        bank_ms = None
        for f in range(0, 60):
            n = int(round((f * HOP - t0) / dec))
            if n >= 0 and d[min(n, len(d) - 1)] >= 0.63:
                bank_ms = f * FRAME_MS
                break
        if bank_ms is None:
            bank_ms = float('nan')

        if fft_ms is None or np.isnan(bank_ms) or bank_ms <= 0:
            sp = "-"
        else:
            sp = "%.1fx" % (fft_ms / bank_ms)
        print("  %3d  %9.1f  %9.2f  %5d  %7.0f   %6.1f ms   %6.1f ms   %s"
              % (k, fc, width, lvl, fs_band,
                 fft_ms if fft_ms is not None else -1, bank_ms, sp))


def cost_table():
    print()
    print("=" * 80)
    print("COST - section-samples per frame (hop 512)")
    print("=" * 80)
    assign = assign_levels(CENTRE31)
    total = 0
    aa_total = 0
    print("  level  rate Hz   bands   band section-samples   AA section-samples")
    for m in range(MAX_LEVEL + 1):
        nb = sum(1 for a in assign if a == m)
        if nb == 0:
            continue
        new_samples = HOP / (2 ** m)
        band_ss = nb * 2 * new_samples
        # the anti-alias filter for the decimation into this level runs at the
        # PREVIOUS level's rate, over that level's new samples
        aa_ss = 2 * (HOP / (2 ** (m - 1))) if m > 0 else 0
        total += band_ss
        aa_total += aa_ss
        print("  %5d  %7.0f   %5d   %17.0f   %17.0f"
              % (m, level_rate(m), nb, band_ss, aa_ss))
    print("  band filters  %8.0f section-samples" % total)
    print("  anti-alias    %8.0f section-samples" % aa_total)
    print("  TOTAL         %8.0f section-samples per frame" % (total + aa_total))
    print()
    print("  The existing 8-band bank: %d section-samples per frame (measured 123 us)."
          % (2 * HOP + 8 * 2 * (HOP // 16)))
    print("  At the same cost per section-sample that is about %.0f us per frame."
          % ((total + aa_total) / float(2 * HOP + 8 * 2 * (HOP // 16)) * 123.0))
    print("  The transform's path costs 2535 us per frame.")


def geq_response_table():
    print()
    print("=" * 80)
    print("THE 16 DISPLAY BANDS - the same question for the half-octave set")
    print("=" * 80)
    assign = assign_levels(CENTRE16)
    print("  band  centre Hz   width Hz   level   rate Hz   FFT 63%    bank 63%   speed-up")
    print("  ----  ---------   ---------  -----   -------   ---------   ---------  ---------")
    for k in range(16):
        fc = CENTRE16[k]
        width = EDGE_HI16[k] - EDGE_LO16[k]
        lvl = assign[k]
        fft_ms = None
        for f in range(0, 60):
            if fft_response(256, f) >= 0.63:
                fft_ms = f * FRAME_MS
                break
        h, fs_band, dec = chain_impulse_band_rate(fc, width, lvl)
        d = detector_response(h, 5.0, fs_band)
        bank_ms = None
        for f in range(0, 60):
            n = int(round((f * HOP - 256) / dec))
            if n >= 0 and d[min(n, len(d) - 1)] >= 0.63:
                bank_ms = f * FRAME_MS
                break
        sp = "-" if (fft_ms is None or bank_ms is None or bank_ms <= 0) else "%.1fx" % (fft_ms / bank_ms)
        print("  %3d  %9.1f  %9.2f  %5d  %7.0f   %6.1f ms   %6.1f ms   %s"
              % (k, fc, width, lvl, fs_band,
                 fft_ms if fft_ms is not None else -1,
                 bank_ms if bank_ms is not None else -1, sp))


if __name__ == "__main__":
    response_table()
    geq_response_table()
    cost_table()

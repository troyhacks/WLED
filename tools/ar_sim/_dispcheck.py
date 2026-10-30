"""Check the display bank's design numbers BEFORE they go into the firmware.

Two things are checked:

  1. The closed-form Q really does put the DIGITAL -3 dB width on the wanted
     width, for all sixteen display bands, with the firmware's own RBJ
     bandpass (not the prewarped form - they are the same filter, but the
     firmware runs the RBJ one).

  2. The 63% response time the firmware will MEASURE (impulse -> cumulative
     energy -> the same one-pole the detector runs) matches the numbers the
     design predicted, so the firmware's measurement is a check on the design
     and not a surprise.

Run:  python _dispcheck.py
"""

import numpy as np

FS = 48000.0
TAU_MS = 5.0

FLOW16, FHIGH16 = 35.0, 11025.0
_R16 = (FHIGH16 / FLOW16) ** (1.0 / 16.0)
CENTRE16 = FLOW16 * _R16 ** (np.arange(16) + 0.5)
EDGE_LO16 = FLOW16 * _R16 ** np.arange(16)
EDGE_HI16 = FLOW16 * _R16 ** (np.arange(16) + 1.0)
WIDTH16 = EDGE_HI16 - EDGE_LO16


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


def rbj_bpf(f0, fs, q):
    """The firmware's bankRbjBpf."""
    w0 = 2.0 * np.pi * f0 / fs
    alpha = np.sin(w0) / (2.0 * q)
    a0 = 1.0 + alpha
    return Biquad(alpha / a0, 0.0, -alpha / a0,
                  -2.0 * np.cos(w0) / a0, (1.0 - alpha) / a0)


def disp_q_for_width(f0, fs, width):
    """The closed form, exactly as the firmware will implement it."""
    u = np.tan(np.pi * f0 / fs)
    return u / ((1.0 + u * u) * np.tan(np.pi * width / fs))


def scan_width(c, fs, n=400001):
    """The measured digital -3 dB width, by a scan of |H|^2."""
    f = np.linspace(0.01, fs * 0.49999, n)
    w = 2 * np.pi * f / fs
    z1 = np.exp(-1j * w)
    z2 = z1 * z1
    num = np.abs(c.b0 + c.b1 * z1 + c.b2 * z2) ** 2
    den = np.abs(1 + c.a1 * z1 + c.a2 * z2) ** 2
    m = num / den
    i = int(np.argmax(m))
    half = m >= 0.5 * m.max()
    lo = i
    while lo > 0 and half[lo - 1]:
        lo -= 1
    hi = i
    while hi < len(m) - 1 and half[hi + 1]:
        hi += 1
    return f[lo], f[hi], f[i], m.max()


def enbw(c, fs, n=2000):
    """The integral of |H|^2 over the spectrum, in Hz."""
    f = np.linspace(0.0, fs * 0.5, n)
    w = 2 * np.pi * f / fs
    z1 = np.exp(-1j * w)
    z2 = z1 * z1
    num = np.abs(c.b0 + c.b1 * z1 + c.b2 * z2) ** 2
    den = np.abs(1 + c.a1 * z1 + c.a2 * z2) ** 2
    return float(np.trapezoid(num / den, f))


def resp63(c, fs, tau_ms, n=8192):
    """The 63% response time, by the firmware's own recursion.

    An impulse into the filter; the detector's expected value rises as the
    cumulative energy of the impulse response; the detector is a one-pole.
    """
    h = np.zeros(n)
    h[0] = 1.0
    w1 = w2 = 0.0
    for i in range(n):
        x = h[i]
        y = c.b0 * x + w1
        w1 = c.b1 * x - c.a1 * y + w2
        w2 = c.b2 * x - c.a2 * y
        h[i] = y
    cum = np.cumsum(h * h)
    alpha = 1.0 - np.exp(-1.0 / (tau_ms * 0.001 * fs))
    det = 0.0
    arr = np.zeros(n)
    for i in range(n):
        det += alpha * (cum[i] - det)
        arr[i] = det
    final = arr[-1]
    idx = int(np.argmax(arr >= 0.63 * final))
    return idx / fs * 1000.0, final


if __name__ == "__main__":
    print("=" * 104)
    print("1. THE CLOSED-FORM Q - does the digital width land on the wanted width?")
    print("=" * 104)
    print("  %-4s %-10s %-9s %-8s %-9s %-9s %-9s %-8s %s"
          % ("band", "centre", "wanted", "Q", "Q plain", "-3dB lo", "-3dB hi",
             "width", "err"))
    worst = 0.0
    for k in range(16):
        fc = CENTRE16[k]
        want = WIDTH16[k]
        q = disp_q_for_width(fc, FS, want)
        c = rbj_bpf(fc, FS, q)
        lo, hi, pk, g = scan_width(c, FS)
        err = (hi - lo) / want - 1.0
        worst = max(worst, abs(err))
        print("  %-4d %-10.2f %-9.3f %-8.4f %-9.4f %-9.3f %-9.3f %-9.3f %+7.3f%%"
              % (k, fc, want, q, fc / want, lo, hi, hi - lo, err * 100))
    print()
    print("  worst error: %.4f%%   (the peak gain at f0 is 1.0 by construction)"
          % (worst * 100))

    print()
    print("=" * 104)
    print("2. THE RESPONSE TIME - what the firmware will measure")
    print("=" * 104)
    print("  %-4s %-10s %-9s %-9s %-11s %-11s %s"
          % ("band", "centre", "width", "ENBW", "resp 63%", "fft 63%", "speed-up"))
    for k in range(16):
        fc = CENTRE16[k]
        want = WIDTH16[k]
        q = disp_q_for_width(fc, FS, want)
        c = rbj_bpf(fc, FS, q)
        e = enbw(c, FS)
        t, _ = resp63(c, FS, TAU_MS)
        print("  %-4d %-10.2f %-9.3f %-9.3f %-11.2f %-11.2f %.2fx"
              % (k, fc, want, e, t, 42.7, 42.7 / t))

    print()
    print("=" * 104)
    print("3. THE COST - 16 sections over the hop")
    print("=" * 104)
    print("  16 sections x 512 samples = %d section-samples a frame" % (16 * 512))
    print("  at the measured ~130 ns a section-sample that is ~%d us"
          % (16 * 512 * 130 // 1000))

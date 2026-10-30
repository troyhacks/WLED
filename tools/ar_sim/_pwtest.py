"""Settle the band filter's shape.

The display's top band is 9211 Hz and the tree's rate there is 48 kHz, so
f0/fs = 0.192. The cookbook's RBJ bandpass (which is what the firmware's
bankRbjBpf implements) is an approximation that is exact for small f0/fs and
falls apart near Nyquist.

This derives the prewarped form and checks what it buys:

    H_a(s) = BW*s / (s^2 + BW*s + w0a^2),  BW = w0a/Q

which has a peak gain of exactly 1 at s = j*w0a and a -3 dB bandwidth of
exactly BW. Bilinear-transforming it with s = (2/T)(1-z^-1)/(1+z^-1) and
prewarping w0a so that the PEAK lands on the wanted digital frequency gives:

    u = tan(pi*f0/fs)                 the prewarp
    a0 = 1 + u/Q + u^2
    b0 =  (u/Q)/a0     b1 = 0    b2 = -(u/Q)/a0
    a1 = (-2 + 2u^2)/a0            a2 = (1 - u/Q + u^2)/a0

With v = tan(w/2) the magnitude is

    |H|^2 = s^2 / (s^2 + Q^2 (1-s^2)^2),   s = v/u

so the peak is at s = 1 (i.e. at w = 2 atan(u), which IS 2*pi*f0/fs because
u = tan(pi*f0/fs)) and the half-power points are s_lo/s_hi from

    s_lo = (sqrt(1+4Q^2) - 1)/(2Q)      s_hi = (sqrt(1+4Q^2) + 1)/(2Q)

Run:  python _pwtest.py
"""

import numpy as np

FS = 48000.0


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


def pw_bpf(f0, fs, q):
    """Prewarped bandpass: the peak lands exactly on 2*pi*f0/fs."""
    u = np.tan(np.pi * f0 / fs)
    a0 = 1.0 + u / q + u * u
    return Biquad((u / q) / a0, 0.0, -(u / q) / a0,
                  (-2.0 + 2.0 * u * u) / a0, (1.0 - u / q + u * u) / a0)


def mag2(c, f, fs):
    w = 2 * np.pi * f / fs
    z1 = np.exp(-1j * w)
    z2 = z1 * z1
    num = c[0] + c[1] * z1 + c[2] * z2
    den = 1 + c[3] * z1 + c[4] * z2
    return abs(num) ** 2 / abs(den) ** 2


def coef(c):
    return (c.b0, c.b1, c.b2, c.a1, c.a2)


def measure(f0, fs, q, which="pw"):
    """The peak, the -3 dB points and the width, scanned strictly below Nyquist."""
    c = pw_bpf(f0, fs, q) if which == "pw" else rbj_bpf(f0, fs, q)
    b0, b1, b2, a1, a2 = coef(c)
    f = np.linspace(0.01, fs * 0.49999, 400001)
    w = 2 * np.pi * f / fs
    z1 = np.exp(-1j * w)
    z2 = z1 * z1
    num = np.abs(b0 + b1 * z1 + b2 * z2) ** 2
    den = np.abs(1 + a1 * z1 + a2 * z2) ** 2
    m = num / den
    pk = f[np.argmax(m)]
    half = m >= 0.5 * m.max()
    i = int(np.argmax(m))
    lo = i
    while lo > 0 and half[lo - 1]:
        lo -= 1
    hi = i
    while hi < len(m) - 1 and half[hi + 1]:
        hi += 1
    return pk, f[lo], f[hi], m.max()


def analytic_width(f0, fs, q):
    """The closed form: -3 dB points from s_lo/s_hi."""
    u = np.tan(np.pi * f0 / fs)
    r = np.sqrt(1.0 + 4.0 * q * q)
    s_lo = (r - 1.0) / (2.0 * q)
    s_hi = (r + 1.0) / (2.0 * q)
    w_lo = 2.0 * np.arctan(s_lo * u)
    w_hi = 2.0 * np.arctan(s_hi * u)
    return w_lo * fs / (2 * np.pi), w_hi * fs / (2 * np.pi)


def solve_q(f0, fs, want_hz, iters=6):
    """Pick Q so the -3 dB width is exactly want_hz."""
    q = f0 / want_hz
    for _ in range(iters):
        lo, hi = analytic_width(f0, fs, q)
        w = hi - lo
        if w <= 0:
            break
        q *= w / want_hz
    return q


if __name__ == "__main__":
    print("=" * 100)
    print("1. THE UNWARPED RBJ - what the firmware has today")
    print("=" * 100)
    print("  %-7s %-8s %-7s %-9s %-9s %-9s %-9s %s"
          % ("fs", "f0", "Q", "peak Hz", "-3dB lo", "-3dB hi", "width", "width/(f0/Q)"))
    for fs, f0, q in ((3000.0, 19.95, 4.3188), (3000.0, 44.67, 5.6421),
                      (48000.0, 9211.0, 2.7701), (48000.0, 6429.3, 2.7672),
                      (48000.0, 3132.4, 2.7676), (48000.0, 4487.6, 2.7674),
                      (48000.0, 41.89, 2.7674), (375.0, 41.89, 2.7674),
                      (24000.0, 2186.4, 2.7674), (6000.0, 1065.2, 2.7674),
                      (750.0, 86.0, 2.7674), (48000.0, 176.0, 2.7674)):
        pk, lo, hi, g = measure(f0, fs, q, "rbj")
        print("  %-7.0f %-8.2f %-7.3f %-9.2f %-9.3f %-9.3f %-9.3f %6.3f   (peak gain %.4f)"
              % (fs, f0, q, pk, lo, hi, hi - lo, (hi - lo) / (f0 / q), g))

    print()
    print("=" * 100)
    print("2. THE PREWARPED FORM - peak exactly on the band centre")
    print("=" * 100)
    print("  %-7s %-8s %-7s %-9s %-9s %-9s %-9s %s"
          % ("fs", "f0", "Q", "peak Hz", "-3dB lo", "-3dB hi", "width", "width/(f0/Q)"))
    for fs, f0, q in ((3000.0, 19.95, 4.3188), (3000.0, 44.67, 5.6421),
                      (48000.0, 9211.0, 2.7701), (48000.0, 6429.3, 2.7672),
                      (48000.0, 3132.4, 2.7676), (48000.0, 4487.6, 2.7674),
                      (48000.0, 41.89, 2.7674), (375.0, 41.89, 2.7674),
                      (24000.0, 2186.4, 2.7674), (6000.0, 1065.2, 2.7674),
                      (750.0, 86.0, 2.7674), (48000.0, 176.0, 2.7674)):
        pk, lo, hi, g = measure(f0, fs, q, "pw")
        alo, ahi = analytic_width(f0, fs, q)
        print("  %-7.0f %-8.2f %-7.3f %-9.2f %-9.3f %-9.3f %-9.3f %6.3f   (peak gain %.4f)"
              % (fs, f0, q, pk, lo, hi, hi - lo, (hi - lo) / (f0 / q), g))
        print("       closed form: lo %.4f hi %.4f  (scan agrees to %.2e)"
              % (alo, ahi, max(abs(alo - lo), abs(ahi - hi))))

    print()
    print("=" * 100)
    print("3. SOLVING Q FOR THE NOMINAL WIDTH")
    print("=" * 100)
    print("  %-7s %-8s %-9s %-9s %-9s %-9s %s"
          % ("fs", "f0", "want Hz", "Q solved", "Q plain", "-3dB lo", "-3dB hi"))
    for fs, f0, want in ((3000.0, 19.95, 4.6185), (3000.0, 44.67, 7.9167),
                         (48000.0, 9211.0, 3325.6), (48000.0, 6429.3, 2321.0),
                         (48000.0, 3132.4, 1129.6), (48000.0, 4487.6, 1620.6),
                         (48000.0, 41.89, 15.135), (375.0, 41.89, 15.135),
                         (24000.0, 2186.4, 790.0), (6000.0, 1065.2, 384.9),
                         (750.0, 86.0, 31.08), (48000.0, 176.0, 63.6)):
        q = solve_q(f0, fs, want)
        pk, lo, hi, g = measure(f0, fs, q, "pw")
        print("  %-7.0f %-8.2f %-9.2f %-9.4f %-9.4f %-9.3f %-9.3f  width %.3f (%.4f of want)"
              % (fs, f0, want, q, f0 / want, lo, hi, hi - lo, (hi - lo) / want))

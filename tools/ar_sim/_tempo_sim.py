#!/usr/bin/env python3
"""Offline check of the tempo-estimation idea, before any of it is written in C.

The claim under test is the one the whole range decision rests on:

    An onset train is periodic, so its autocorrelation peaks at the beat AND at
    every SUBDIVISION of it - at multiples of the fundamental period, never at
    half of it. The catalogue tempo (the number a Pioneer deck shows) is a
    harmonic of the finest percussive grid, so the search band has to contain
    that harmonic and exclude its neighbours.

Two things follow, and the simulator checks both:

  1. The ODF must see the WHOLE percussive grid, hats included. A kick-only
     ODF sees the kick pattern, whose fundamental may be the half-time tempo,
     and the catalogue tempo is then not a peak at all - there is nothing for
     any estimator to find.

  2. The band must have ratio below 2, so that no two octaves of the same
     tempo are both candidates.

    python tools/ar_sim/_tempo_sim.py                # the table
    python tools/ar_sim/_tempo_sim.py --odf sub      # sub-hop ODF
    python tools/ar_sim/_tempo_sim.py --skip          # injected skipped beats
    python tools/ar_sim/_tempo_sim.py --sweep         # band sweep
"""

import argparse
import math
import sys

import numpy as np

SR = 48000

# The AR's own 31 ISO 266 third-octave bands, 19.95 Hz .. 19952.6 Hz.
ISO_BANDS = [10 ** ((13 + k) * 0.1) for k in range(31)]


# --------------------------------------------------------------------------
# Synthetic percussion
# --------------------------------------------------------------------------


def _env(n, decay_ms):
    return np.exp(-np.arange(n) / (decay_ms * 1e-3 * SR))


def hit_kick(n=4096):
    t = np.arange(n) / SR
    f = 52 + 90 * np.exp(-t / 0.020)
    phase = 2 * np.pi * np.cumsum(f) / SR
    body = np.sin(phase) * _env(n, 120)
    click = np.random.randn(n) * np.exp(-np.arange(n) / (2 * 1e-3 * SR)) * 0.25
    return body + click


def hit_snare(n=4096):
    noise = np.random.randn(n)
    noise = np.diff(np.concatenate(([0.0], noise)))
    tone = np.sin(2 * np.pi * 185 * np.arange(n) / SR) * 0.4
    return (noise * 0.6 + tone) * _env(n, 110)


def hit_hat(n=2048):
    noise = np.diff(np.concatenate(([0.0], np.random.randn(n))))
    return noise * _env(n, 28) * 0.35


def hit_clap(n=4096):
    noise = np.random.randn(n)
    noise = np.diff(np.concatenate(([0.0], noise)))
    burst = np.zeros(n)
    for off in (0, 220, 440):
        burst[off:off + 260] += np.random.randn(260) * 0.6
    tail = noise * np.exp(-np.arange(n) / (0.09 * SR)) * 0.35
    return (burst + tail) * 0.5


def hit_bass(n=8192, f=55.0):
    t = np.arange(n) / SR
    return np.sin(2 * np.pi * f * t) * _env(n, 260) * 0.5


def _place(buf, hit, at, gain=1.0):
    i = int(at * SR)
    if i >= len(buf):
        return
    seg = hit[: min(len(hit), len(buf) - i)] * gain
    buf[i:i + len(seg)] += seg


STYLES = ["house", "trap", "dnb", "hiphop", "garage", "hardstyle"]


def _seed(style, bpm):
    """A stable seed - hash() is salted per process, so results must not
    depend on it."""
    return (sum(ord(c) for c in style) * 131 + int(bpm) * 17) % 9973


def make_track(bpm, seconds=16.0, style="house", seed=1):
    """A synthetic track with a known catalogue tempo.

    The styles differ in WHERE the onsets fall, because that is the whole
    question. A four-on-the-floor kick makes the fundamental BE the catalogue
    tempo; a half-time drum makes the fundamental the half-time tempo, and
    the catalogue tempo is then a harmonic of the finest grid instead.
    """
    np.random.seed(seed)
    n = int(seconds * SR)
    buf = np.zeros(n + SR)
    beat = 60.0 / bpm
    nbeats = int(seconds / beat)

    kick, snare, hat, clap = hit_kick(), hit_snare(), hit_hat(), hit_clap()

    for b in range(nbeats):
        t = b * beat
        inbar = b % 4

        if style == "house":
            _place(buf, kick, t, 1.0)                       # 4 on the floor
            if inbar in (1, 3):
                _place(buf, clap, t, 0.55)
            if b % 2 == 0:
                _place(buf, hit_bass(f=55.0), t, 0.5)
            for h in range(2):                              # 8th hats
                _place(buf, hat, t + h * beat / 2, 0.5)

        elif style == "trap":
            # HALF TIME: kick every two beats, snare on 3, 8th hats.
            # The kick's fundamental is bpm/2. The catalogue tempo is bpm,
            # and it only exists as a harmonic because of the hats.
            if b % 2 == 0:
                _place(buf, kick, t, 1.0)
                _place(buf, hit_bass(f=45.0), t, 0.55)
            if inbar == 2:
                _place(buf, snare, t, 0.8)
            for h in range(2):
                _place(buf, hat, t + h * beat / 2, 0.6)

        elif style == "dnb":
            if inbar in (0, 2):
                _place(buf, kick, t, 0.95)
                _place(buf, hit_bass(f=48.0), t, 0.6)
            if inbar in (1, 3):
                _place(buf, snare, t, 0.85)
            for h in range(4):                              # 16th hats
                _place(buf, hat, t + h * beat / 4, 0.45)

        elif style == "hiphop":
            if inbar in (0, 2):
                _place(buf, kick, t, 0.95)
            if inbar in (1, 3):
                _place(buf, snare, t, 0.85)
            for h in range(2):
                _place(buf, hat, t + h * beat / 2, 0.5)

        elif style == "garage":
            if inbar in (0, 1):
                _place(buf, kick, t, 0.9)
            if inbar == 2:
                _place(buf, snare, t, 0.8)
            _place(buf, hat, t + beat * 0.66, 0.55)          # the shuffle
            _place(buf, hat, t + beat / 2, 0.45)

        elif style == "hardstyle":
            _place(buf, kick, t, 1.0)                        # kick every beat
            if inbar in (1, 3):
                _place(buf, clap, t, 0.5)
            for h in range(4):                              # 16th hats
                _place(buf, hat, t + h * beat / 4, 0.5)

        else:
            raise ValueError(style)

    buf = buf[:n]
    peak = np.max(np.abs(buf))
    if peak > 0:
        buf = buf / peak * 0.7
    return buf


# --------------------------------------------------------------------------
# The onset detection function
# --------------------------------------------------------------------------


def band_powers(x, hop, centres):
    """Band powers per hop, the way a filter bank would measure them.

    FFT-based here for speed; the firmware uses an IIR bank. What matters for
    the ODF is the TIME structure, and an FFT bank and a filter bank agree on
    that to within the window length.
    """
    n = (len(x) // hop) * hop
    frames = x[:n].reshape(-1, hop)
    w = np.hanning(hop)
    F = np.fft.rfft(frames * w, axis=1)
    P = (np.abs(F) ** 2)
    df = SR / hop
    freqs = np.fft.rfftfreq(hop, 1.0 / SR)

    out = np.zeros((len(frames), len(centres)))
    for k, fc in enumerate(centres):
        lo = fc / 2 ** (1 / 6.0)     # third octave
        hi = fc * 2 ** (1 / 6.0)
        sel = (freqs >= lo) & (freqs < hi)
        if not np.any(sel):
            sel = np.argmin(np.abs(freqs - fc)) == np.arange(len(freqs))
        out[:, k] = P[:, sel].sum(axis=1)
    return out


def odf_multiband(x, hop, centres):
    """Spectral flux over the bands: the ODF the AR could get for free.

    The AR already measures these band powers every frame, so this ODF costs
    one subtraction per band per frame and nothing else.
    """
    P = band_powers(x, hop, centres)
    P = np.maximum(P, 1e-20)
    L = np.log(P)
    flux = np.maximum(np.diff(L, axis=0, prepend=L[:1]), 0.0)
    return flux.sum(axis=1)


def odf_kick_only(x, hop):
    """The ODF a low-band-only tracker would see. Kept for comparison."""
    y = np.fft.irfft(
        np.fft.rfft(x) * ((np.fft.rfftfreq(len(x), 1 / SR) >= 30) &
                          (np.fft.rfftfreq(len(x), 1 / SR) <= 200)).astype(float),
        n=len(x))
    n = (len(y) // hop) * hop
    env = np.sqrt((y[:n].reshape(-1, hop) ** 2).mean(axis=1) + 1e-20)
    db = 20 * np.log10(env)
    return np.maximum(np.diff(db, prepend=db[0]), 0.0)


# --------------------------------------------------------------------------
# Estimation
# --------------------------------------------------------------------------


def acf_peak(odf, rate, bpm_min, bpm_max, window_s):
    """Coarse period: the ACF peak inside the band, with parabolic refinement."""
    n = int(window_s * rate)
    if len(odf) < n:
        return None, 0.0
    x = odf[-n:]
    x = x - x.mean()
    x = x * np.hanning(len(x))
    nfft = 1 << int(math.ceil(math.log2(2 * len(x))))
    X = np.fft.rfft(x, nfft)
    r = np.fft.irfft(np.abs(X) ** 2, nfft)[: len(x)]
    r /= max(r[0], 1e-20)

    lo = max(1, int(60.0 * rate / bpm_max))
    hi = min(len(r) - 2, int(60.0 * rate / bpm_min))
    if hi <= lo:
        return None, 0.0
    w = r[lo:hi + 1]
    k = int(np.argmax(w))
    lag = lo + k
    if 0 < k < len(w) - 1:
        a, b, c = w[k - 1], w[k], w[k + 1]
        den = a - 2 * b + c
        if abs(den) > 1e-12:
            d = 0.5 * (a - c) / den
            if abs(d) < 1.0:
                lag += d
    return 60.0 * rate / lag, float(w[k])


def pll_refine(odf, rate, bpm0, win_s=1.0, kp=0.25, ki=0.02):
    """A PLL on the ODF's own periodicity, not on individual onsets.

    The phase detector is the complex DFT of the ODF at the current beat
    frequency. Because it works on the CONTINUOUS signal, a beat whose onset
    is missing does not shift the phase - the estimate coasts on the beats
    that are there. That is the answer to skipped beats.

    Returns (bpm, coherence, phase_offset_beats).
    """
    if not bpm0 or bpm0 <= 0:
        return bpm0, 0.0, 0.0

    f = bpm0 / 60.0
    win = int(win_s * rate)
    if len(odf) < 2 * win:
        return bpm0, 0.0, 0.0

    w = np.hanning(win)
    phi_hat = 0.0
    integ = 0.0
    coh = 0.0
    t = np.arange(win)

    # slide the window a quarter of a window at a time
    step = max(1, win // 4)
    starts = range(0, len(odf) - win, step)

    for s in starts:
        seg = odf[s:s + win]
        # the DFT of the window at the current frequency, time origin at the
        # window centre
        z = np.sum(seg * w * np.exp(-2j * np.pi * f * (t - win / 2) / rate))
        mag = abs(z)
        if mag < 1e-20:
            continue
        energy = np.sum(seg ** 2 * w)
        coh = mag / max(energy, 1e-20)
        ph = np.angle(z)
        e = ph - phi_hat
        e = (e + np.pi) % (2 * np.pi) - np.pi        # wrap to +-pi
        integ += ki * e
        phi_hat = (phi_hat + kp * e) % (2 * np.pi)
        # a phase that runs ahead means the true frequency is higher
        f += (integ) * 0.5 / win

    return f * 60.0, coh, (phi_hat / (2 * np.pi)) % 1.0


def estimate(x, hop, band, window_s, use_pll, skip=None):
    odf = odf_multiband(x, hop, ISO_BANDS)
    if skip:
        odf = inject_skips(odf, rate=SR / hop, skip=skip)
    bpm, conf = acf_peak(odf, SR / hop, band[0], band[1], window_s)
    if bpm is None:
        return None, 0.0, 0.0
    if use_pll:
        bpm, coh, _ = pll_refine(odf, SR / hop, bpm)
        return bpm, conf, coh
    return bpm, conf, 0.0


def inject_skips(odf, rate, skip=0.25, seed=3):
    """Blank a fraction of the ODF - the 'skipped beats' failure mode.

    A beat that produces no onset must not shift the reported phase or make
    the estimate jump. Here we remove whole BEATS worth of ODF energy.
    """
    rng = np.random.RandomState(seed)
    out = odf.copy()
    beat = int(rate * 0.5)               # ~120 BPM
    n = len(out) // beat
    for i in range(n):
        if rng.rand() < skip:
            out[i * beat:(i + 1) * beat] = 0.0
    return out


# --------------------------------------------------------------------------
# The experiment
# --------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--style", default=None)
    ap.add_argument("--odf", default="frame", choices=["frame", "sub"])
    ap.add_argument("--band", default="90-175")
    ap.add_argument("--window", type=float, default=12.0)
    ap.add_argument("--no-pll", action="store_true")
    ap.add_argument("--skip", type=float, default=0.0,
                    help="fraction of beats to blank (skipped beats)")
    ap.add_argument("--sweep", action="store_true")
    args = ap.parse_args()

    bpm_min, bpm_max = (float(v) for v in args.band.split("-"))
    band = (bpm_min, bpm_max)
    hop = 128 if args.odf == "sub" else 512
    styles = [args.style] if args.style else STYLES
    use_pll = not args.no_pll

    if args.sweep:
        sweep(band, hop, args.window, use_pll)
        return 0

    ratio = bpm_max / bpm_min
    print("band [%g, %g]  ratio %.3f  %s" % (
        bpm_min, bpm_max, ratio,
        "OCTAVE-UNAMBIGUOUS" if ratio < 2.0 else "AMBIGUOUS"))
    print("ODF %.1f Hz (%d bands)   window %.1f s   PLL %s   skips %.0f%%" % (
        SR / hop, len(ISO_BANDS), args.window, "on" if use_pll else "off",
        args.skip * 100))
    print()
    print("%-10s %6s %10s %8s %6s %s" % (
        "style", "bpm", "estimated", "error", "conf", "verdict"))

    fails = 0
    for style in styles:
        for bpm in (90, 100, 120, 128, 140, 150, 160, 174):
            est, conf, coh = estimate(
                make_track(bpm, style=style, seed=_seed(style, bpm)),
                hop, band, args.window, use_pll, args.skip or None)
            if est is None:
                print("%-10s %6d %10s %8s %6.2f NO ESTIMATE" % (style, bpm, "-", "-", conf))
                fails += 1
                continue
            err = est - bpm
            ok = abs(err) < 2.0
            print("%-10s %6d %10.2f %8.2f %6.2f %s" % (
                style, bpm, est, err, conf, "ok" if ok else "WRONG"))
            if not ok:
                fails += 1

    print()
    print("%d wrong" % fails)
    return 0


def sweep(band, hop, window_s, use_pll):
    """Which bands get the catalogue tempo right?"""
    bpms = (90, 100, 120, 128, 140, 150, 160, 174)
    print("%-12s %6s %6s  %s" % ("band", "ratio", "wrong", "per-style wrong counts"))
    for lo, hi in ((90, 175), (90, 150), (100, 175), (95, 185),
                   (100, 145), (110, 145), (120, 140), (70, 180), (60, 200)):
        counts = {}
        wrong = 0
        for style in STYLES:
            c = 0
            for bpm in bpms:
                est, _, _ = estimate(
                    make_track(bpm, style=style, seed=_seed(style, bpm)),
                    hop, (lo, hi), window_s, use_pll)
                if est is None or abs(est - bpm) >= 2.0:
                    c += 1
            counts[style] = c
            wrong += c
        detail = " ".join("%s:%d" % (s[:4], c) for s, c in counts.items())
        print("%-12s %6.3f %6d  %s" % (
            "%g-%g" % (lo, hi), hi / lo, wrong, detail))
    return 0


if __name__ == "__main__":
    sys.exit(main())

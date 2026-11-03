#!/usr/bin/env python3
"""THE FIRMWARE'S ODF, FAITHFULLY.  The emulation's band was wrong.

WHAT WAS WRONG
--------------
`_device_odf.device_odf()` builds the "device" ODF like this:

    d = T.designs(fs=fs)          # fs = 3000
    for c in secs:                # the filters are DESIGNED for 3 kHz ...
        y = lfilter(...)          # ... but RUN on the 6 kHz cache
    y = y[::dec]                  # and only then decimated

A digital filter designed for a corner f at sample rate fs has that corner at
f * (the rate it runs at / fs) when it runs at a different rate.  The
40-160 Hz filters, run at 6 kHz, are really an 80-320 Hz band.  The corpus
was therefore measured against a band the firmware will NOT have.

The firmware runs the coefficients at their design rate: a Butterworth
anti-alias at 300 Hz on the 48 kHz stream, a decimation by 16 to 3 kHz, and
THEN the 40-160 Hz band filters at 3 kHz.  So the band really is 40-160 Hz,
and the kick fundamental - 45-70 Hz in house music - is inside it.

THE FIX
-------
    the anti-alias at 300 Hz, on the 6 kHz cache (the same analog response:
        a Butterworth's magnitude is scale-invariant, so a 300 Hz corner is
        a 300 Hz corner at any rate)
    decimate by 2 to 3 kHz
    the 40-160 Hz band filters, AT 3 kHz

The decimation's alias rejection: the firmware's 4th-order Butterworth at
300 Hz is -50 dB at 3 kHz, so the 3-6 kHz content that folds back is 50 dB
down.  The emulation's own is the same filter, so it is the same number.

    python tools/ar_sim/_device_odf2.py --limit 25
"""

import argparse
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _tempo_acf as T                             # noqa: E402
from _device_odf import DEVICE_FS, DEVICE_HOP, DEVICE_RATE  # noqa: E402


def butter_lp_sections(fc, fs, order, max_secs):
    """The AR's bankButterLowpass(), in Python.  The same file, the same
    convention: {b0, b1, b2, a1, a2} with a0 = 1, and the cascade's gain at
    DC is 1.

    This is a TRANSCRIPTION of usermods/audioreactive/audio_reactive.h's
    bankButterLowpass(), so the emulation's anti-alias filter is the
    firmware's anti-alias filter and not a lookalike.
    """
    K = math.tan(math.pi * fc / fs)
    K2 = K * K
    out = []
    for k in range(order):
        if len(out) >= max_secs:
            break
        theta = math.pi * (order + 2.0 * k + 1.0) / (2.0 * order)
        px = math.cos(theta)
        py = math.sin(theta)
        if py < -1e-6:
            continue                    # the conjugate of a pole already placed
        sigma = -px
        if abs(py) < 1e-6:
            A = 1.0 + sigma * K
            out.append([K / A, K / A, 0.0, (1.0 - sigma * K) / A, 0.0])
        else:
            A = 1.0 / K2 + 2.0 * sigma / K + 1.0
            out.append([1.0 / A, 2.0 / A, 1.0 / A,
                        (2.0 - 2.0 / K2) / A,
                        (1.0 / K2 - 2.0 * sigma / K + 1.0) / A])
    return out


def device_odf_fw(x6k, aa_hz=300.0):
    """The firmware's ODF: the 40-160 Hz band, at 3 kHz, the way the board
    will run it.

    x6k is the 6 kHz cache.  The output is one ODF value per frame, at
    exactly 93.75 frames a second.
    """
    from scipy.signal import lfilter
    y = np.asarray(x6k, dtype=np.float64)

    # the anti-alias filter: the AR's own Butterworth, 4th order
    for c in butter_lp_sections(aa_hz, _cache.CACHE_RATE, 4, 2):
        b0, b1, b2, a1, a2 = c
        y = lfilter([b0, b1, b2], [1.0, a1, a2], y)

    # the decimation: 6 kHz -> 3 kHz
    y = y[::2]

    # the band, AT 3 kHz
    for c in T.designs(fs=DEVICE_FS)["hp40_lp160x2"]:
        b0, b1, b2, a1, a2 = c
        y = lfilter([b0, b1, b2], [1.0, a1, a2], y)

    # the frames: 32 samples each
    n = (len(y) // DEVICE_HOP) * DEVICE_HOP
    frames = y[:n].reshape(-1, DEVICE_HOP)
    p = (frames ** 2).mean(axis=1)
    db = 10.0 * np.log10(p + 1e-12)
    return np.maximum(np.diff(db, prepend=db[0]), 0.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=25)
    args = ap.parse_args()

    from _device_odf import device_odf
    for k in _cache.keys()[:args.limit]:
        x = _cache.load(k)
        if x is None:
            continue
        a = device_odf(x)
        b = device_odf_fw(x)
        print("  %-44s frames %5d %5d   max %8.4f %8.4f   mean %8.5f %8.5f"
              % (k[:44], len(a), len(b), a.max(), b.max(), a.mean(), b.mean()))


if __name__ == "__main__":
    sys.exit(main())

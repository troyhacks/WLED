#!/usr/bin/env python3
"""THE BAND, ON TRACKS WHOSE TEMPO IS KNOWN.

WHY THIS SCRIPT EXISTS
----------------------
On the real corpus the PRIOR decides the answer and the band changes
nothing: every band from 70-140 to 70-176 produced the SAME 83 right and
the same 13 wrong.  With the prior switched off, the corpus collapsed into
4:3 errors (56 of 100), because a house track's beat sits at 4:3 to its own
half-time and the comb has no way to prefer one.

That makes the corpus useless for the range question: the corpus's prior is
the same thing as the corpus's answer, because the corpus is house music
with a median of 127.8 BPM and the prior is centred at 125.

So the band has to be measured on tracks whose tempo is KNOWN, across
tempi that SPAN the band.  The synthetic generator makes exactly those:
`make_track(bpm, style=...)` puts onsets on a grid of the catalogue tempo,
in six styles whose onsets land on different subdivisions of it.

WHAT IT MEASURES
----------------
For each (band, prior) the comb tracker runs on the synthetic track, and
the result is classified against the truth:

    right      within 3 % of the catalogue tempo
    octave     exactly 2x or 0.5x
    3:2        1.5x or 2/3x
    4:3        1.333x or 0.75x
    other      something else

The styles matter: 'house' is four-on-the-floor (the beat IS the catalogue
tempo), 'dnb' is half-time (the catalogue tempo is the double of the
fundamental), 'trap' is at half speed with 16ths on the beat, etc.  A
band that only works for house is not a band.

    python tools/ar_sim/_tempo_band_synth.py --limit 6
"""

import argparse
import math
import os
import sys

import numpy as np
from scipy.signal import lfilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _fw_port as P                               # noqa: E402
from _device_odf2 import butter_lp_sections       # noqa: E402
from _tempo2_sim import STYLES, make_track, _seed  # noqa: E402

SR = 48000
HOP = 512
RATE = SR / HOP            # 93.75


def fw_odf(x48, lo=80.0, hi=320.0):
    """The firmware's ODF on a 48 kHz signal, in the firmware's ORDER.

    the anti-alias filter at 300 Hz, on the 48 kHz stream
    one sample in sixteen is kept    ->  3 kHz
    the band filters, AT 3 kHz      ->  the true 80-320 Hz band
    """
    y = np.asarray(x48, dtype=np.float64)
    for c in butter_lp_sections(300.0, SR, 4, 2):
        b0, b1, b2, a1, a2 = c
        y = lfilter([b0, b1, b2], [1.0, a1, a2], y)
    y = y[::16]                                   # 48 kHz -> 3 kHz
    for c in (P.rbj_hpf(lo, 3000.0, P.Q), P.rbj_hpf(lo, 3000.0, P.Q),
              P.rbj_lpf(hi, 3000.0, P.Q), P.rbj_lpf(hi, 3000.0, P.Q)):
        b0, b1, b2, a1, a2 = c
        y = lfilter([b0, b1, b2], [1.0, a1, a2], y)
    n = (len(y) // 32) * 32
    if n == 0:
        return np.zeros(0)
    p = (y[:n].reshape(-1, 32) ** 2).mean(axis=1)
    db = 10.0 * np.log10(p + 1e-12)
    return np.maximum(np.diff(db, prepend=db[0]), 0.0)


def classify(r):
    if r <= 0:
        return "none"
    if 0.97 <= r <= 1.03:
        return "right"
    for (target, name) in ((2.0, "octave"), (0.5, "octave"),
                           (1.5, "3:2"), (0.6667, "3:2"),
                           (1.3333, "4:3"), (0.75, "4:3")):
        if abs(r - target) < 0.05:
            return name
    return "other"


def configure(lo, hi, prior_weight, prior_bpm=125.0, prior_sigma=0.35):
    P.BPM_LO = float(lo)
    P.BPM_HI = float(hi)
    P.CAND_STEP = 0.25
    P.CAND_COUNT = int(round((float(hi) - float(lo)) / 0.25)) + 1
    P.PRIOR_BPM = float(prior_bpm)
    P.PRIOR_SIGMA = float(prior_sigma)
    P.PRIOR_WEIGHT = float(prior_weight)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=6)
    ap.add_argument("--seconds", type=float, default=16.0)
    args = ap.parse_args()

    # the tempi to test: they SPAN the band, and each one has an octave and
    # a 3:2 partner inside it.
    bpms = [70, 80, 88, 90, 95, 100, 110, 118, 120, 128, 135, 140, 150,
            160, 174, 176]

    bands = [
        ("90-175 +prior", 90.0, 175.0, 0.6),
        ("90-175 noprior", 90.0, 175.0, 0.0),
        ("118-176 +prior", 118.0, 176.0, 0.6),
        ("118-176 noprior", 118.0, 176.0, 0.0),
        ("120-176 +prior", 120.0, 176.0, 0.6),
        ("70-140 +prior", 70.0, 140.0, 0.6),
        ("70-140 noprior", 70.0, 140.0, 0.0),
        ("70-176 +prior", 70.0, 176.0, 0.6),
    ]

    # the ODF, once per (style, bpm)
    print("building the ODFs...")
    odfs = {}
    for style in STYLES:
        for bpm in bpms:
            x = make_track(bpm, seconds=args.seconds, style=style,
                           seed=_seed(style, bpm))
            odfs[(style, bpm)] = fw_odf(x)
    print("  %d tracks" % len(odfs))

    for (label, lo, hi, pw) in bands:
        kinds = {}
        per_style = {}
        # ONLY the tracks whose truth is INSIDE the band count.  A band
        # cannot be blamed for a tempo it was never asked about: 70 BPM is
        # outside 118-176 by construction, so comparing the bands on all
        # the tempi at once compares their SIZE, not their quality.
        for style in STYLES:
            sk = {}
            for bpm in bpms:
                if not (lo <= bpm <= hi):
                    continue
                odf = odfs[(style, bpm)]
                if len(odf) < 200:
                    continue
                configure(lo, hi, pw)
                t = P.Tracker()
                t.run(odf)
                c = classify(t.bpm / bpm if t.bpm > 0 else 0.0)
                kinds[c] = kinds.get(c, 0) + 1
                sk[c] = sk.get(c, 0) + 1
            per_style[style] = sk
        n = sum(kinds.values())
        parts = " ".join("%s %d" % (k, v) for k, v in sorted(kinds.items(),
                                                             key=lambda t: -t[1]))
        # the octave-free margin, g = Bmin - Bmax/2
        g = lo - hi / 2.0
        print("\n%s   n=%d   g=%+.1f   %s" % (label, n, g, parts))
        for style in STYLES:
            sk = per_style[style]
            sp = " ".join("%s %d" % (k, v) for k, v in sorted(sk.items(),
                                                             key=lambda t: -t[1]))
            print("    %-9s %s" % (style, sp))
    return 0


if __name__ == "__main__":
    sys.exit(main())

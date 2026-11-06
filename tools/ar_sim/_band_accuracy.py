#!/usr/bin/env python3
"""THE BAND: how well the tracker holds a rate across the whole range.

Troy's report: "it's better at adjusting small numbers vs large numbers".
This asks the tracker to hold each rate in turn, on a click track at that
exact rate, and reports what it published.

    python tools/ar_sim/_band_accuracy.py
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _fw_port as P                               # noqa: E402
from _fw_band import firmware_odf                  # noqa: E402
from _half_tempo import click_track                # noqa: E402

RATES = [95, 100, 105, 110, 115, 120, 125, 128, 130, 135, 140, 145, 150, 155, 160, 165, 170]
MODES = [(0, 0.0, "the plain grid"), (1, 0.01, "the lattice")]


def held(bpm, quant, bonus):
    odf = firmware_odf(click_track(bpm), P.BAND_LO, P.BAND_HI)
    t = P.Tracker()
    t.quant = quant
    t.whole_bonus = bonus
    t.run(odf)
    log = np.array(t.bpm_log)
    if len(log) < 200:
        return None
    tr = log[-int(20.0 * P.RATE):]
    return float(np.median(tr)), float(np.std(tr))


def main():
    print("%5s | %-22s | %-22s" % ("true", MODES[0][2], MODES[1][2]))
    for bpm in RATES:
        cells = []
        for q, b, _ in MODES:
            r = held(bpm, q, b)
            cells.append("%8.2f  err %+6.2f  sd %.3f" % (r[0], r[0] - bpm, r[1]) if r else "no estimate")
        print("%5d | %-22s | %-22s" % (bpm, cells[0], cells[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())

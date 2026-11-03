#!/usr/bin/env python3
"""THE PHASE'S BIAS - is the grid on the beat, or early?

The corpus metric removes a constant offset before it scores, so it cannot
see a grid that is anchored early by a constant.  This measures the RAW
signed distance from every published beat to the nearest kick onset, and
reports the median.  A grid that sits ON the kick reads near zero; a grid
that is anchored on the plateau's LEADING EDGE reads negative - early.

    python tools/ar_sim/_fw_phase.py --windows 25
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402


def make(plateau, policy='time', frac=0.09, min_score=0.0):
    t = P.Tracker()
    t.narrow_bpm = 2.0
    t.search_every = 4
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = 32
    t.net_margin = 0.70
    t.pll_gain = 0.20
    t.prior_weight = 0.6
    t.prior = t._prior_table()
    t.prior_in_narrow = False
    t.kernel_policy = policy
    t.kernel_frac = frac
    t.plateau = plateau
    t.min_score = min_score
    return t


def raw_bias(beats, onsets):
    """The signed distance from each beat to the nearest onset, in ms."""
    b = np.asarray(beats, dtype=float)
    o = np.asarray(onsets, dtype=float)
    if len(b) == 0 or len(o) == 0:
        return None
    d = b[:, None] - o[None, :]
    j = np.argmin(np.abs(d), axis=1)
    return d[np.arange(len(b)), j] * 1000.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=25)
    args = ap.parse_args()

    for plateau in ("lead", "centre"):
        biases, spreads = [], []
        n = 0
        for k in _cache.keys()[:args.windows]:
            x = _cache.load(k)
            if x is None:
                continue
            odf = B.firmware_odf(x, 90.0, 320.0)
            if len(odf) < 1200:
                continue
            ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
            if len(ons) < 4:
                continue
            t = make(plateau)
            t.run(odf)
            if not t.beats:
                continue
            b = raw_bias(np.array(t.beats) / P.RATE, ons)
            if b is None:
                continue
            biases.append(float(np.median(b)))
            spreads.append(float(np.std(b)))
            n += 1
        a = np.array(biases)
        s = np.array(spreads)
        print("%-7s | the grid's bias vs the kick: median %+7.1f ms  "
              "mean %+7.1f ms | the spread %6.1f ms | %d windows"
              % (plateau, float(np.median(a)), float(a.mean()),
                 float(np.mean(s)), n), flush=True)


if __name__ == "__main__":
    sys.exit(main())

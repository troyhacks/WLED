#!/usr/bin/env python3
"""REVIEW CHECK 3: what does the veto actually SEE on a clean 131 BPM train?

The synthetic run reported 48 vetoes and 0 far wins, which does not match
the score profile (the truth scores 1.00 at 131, the window's best 0.44).
So: print the veto's own trace - the near rate and score, the far rate and
score, and the ratio - and find out why the far is refused.

    python tools/ar_sim/_rev_veto_debug.py
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _fw_port as P                               # noqa: E402

RATE = P.RATE


def beat_odf(bpm, seconds, noise=0.0, seed=0):
    n = int(seconds * RATE)
    odf = np.full(n, noise, dtype=np.float64)
    rng = np.random.RandomState(seed)
    if noise:
        odf += rng.uniform(0.0, noise, n)
    period = 60.0 * RATE / bpm
    t = 0.0
    while t < n:
        i = int(round(t))
        if 0 <= i < n:
            odf[i] += 1.0
        t += period
    return odf


def place_locked(t, bpm, phase=0.0):
    t.locked = True
    t.period = 60.0 * RATE / bpm
    t.bpm = bpm
    t.next_beat = phase
    t.last_beat = phase
    t.beat_index = 0
    t.last_sweep_frame = -1000
    t.last_full_frame = -1000


def main():
    truth = 131.0
    odf = beat_odf(truth, 20.0, noise=0.02, seed=1)

    t = P.Tracker()
    t.narrow_bpm = 2.0
    t.search_every = 4
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = 32
    t.net_margin = 0.20
    t.veto_on_collapse = True
    t.pll_gain = 0.10
    t.exact_kernel = True
    place_locked(t, 125.0)
    t.run(odf)

    print("whole-band searches %d   vetoes %d   far wins %d"
          % (t.search_full, t.net_vetoes, t.net_far))
    print("the veto's own trace (the last 24 decisions):")
    print("  %7s %9s %9s %9s %9s %8s %8s"
          % ("frame", "grid", "near", "near sc", "far", "far sc", "ratio"))
    for r in t.trace[-24:]:
        print("  %7d %9.2f %9.2f %9.4f %9.2f %8.4f %8.3f  %s"
              % (r["frame"], r["bpm"], r["local_bpm"], r["local_score"],
                 r["far_bpm"], r["far_score"], r["ratio"],
                 "FAR" if r["took_far"] else "vetoed"))
    print()
    print("the FIRST 10 decisions:")
    for r in t.trace[:10]:
        print("  %7d %9.2f %9.2f %9.4f %9.2f %8.4f %8.3f  %s"
              % (r["frame"], r["bpm"], r["local_bpm"], r["local_score"],
                 r["far_bpm"], r["far_score"], r["ratio"],
                 "FAR" if r["took_far"] else "vetoed"))
    print()
    print("the number of peaks the comb saw at each decision: "
          "%s" % [r["peaks"] for r in t.trace[:12]])


if __name__ == "__main__":
    sys.exit(main())

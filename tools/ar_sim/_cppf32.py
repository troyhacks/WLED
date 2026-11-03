#!/usr/bin/env python3
"""THE C++'S WINDOW vs THE PORT'S, WHEN arTempo.bpm IS A float32.

The C++'s arTempo.bpm is a float and the window's edge is computed with
ceilf/floorf on float arithmetic; the port's self.bpm is a double.  The
adopted rate is a GRID candidate (90 + i*0.25), so right after a lock the
grid's rate sits EXACTLY on a grid point - which is exactly where the
window's edge is decided.

This measures how often the two windows differ, and whether it changes the
veto's decision.

    python tools/ar_sim/_cppf32.py --windows 8
"""

import argparse
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402

NARROW = 2.0


def window(bpm, step, count):
    lo = max(P.BPM_LO, bpm - NARROW)
    hi = min(P.BPM_HI, bpm + NARROW)
    f = int(math.ceil((lo - P.BPM_LO) / step))
    c = int(math.floor((hi - P.BPM_LO) / step)) - f + 1
    if f < 0:
        f = 0
    if c < 1:
        c = 1
    if f + c > count:
        c = count - f
    return f, c


class Probe(P.Tracker):
    """Records the window, computed both ways, at every whole-band search."""

    def __init__(self):
        super().__init__()
        self.diffs = []
        self.seen = 0

    def sweep_start(self):
        if (not self.locked) or self.bpm <= 0.0:
            super().sweep_start()
            return
        f64 = window(self.bpm, self.cand_step, self.cand_count)
        f32 = window(float(np.float32(self.bpm)), self.cand_step, self.cand_count)
        self.seen += 1
        if f64 != f32:
            self.diffs.append((self.frame_index, self.bpm,
                               float(np.float32(self.bpm)), f64, f32))
        super().sweep_start()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=8)
    args = ap.parse_args()

    tot = 0
    bad = 0
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, 80.0, 320.0)
        if len(odf) < 1200:
            continue
        t = Probe()
        t.narrow_bpm = NARROW
        t.search_every = 4
        t.edge_rule = False
        t.collapse_trigger = True
        t.full_every = 32
        t.net_margin = 0.20
        t.run(odf)
        tot += t.seen
        bad += len(t.diffs)
        print("%-44s whole-band searches %4d   windows differing %3d"
              % (k[:44], t.seen, len(t.diffs)))
        for d in t.diffs[:6]:
            print("      frame %6d  bpm %.6f (f32 %.6f)   f64 %s   f32 %s"
                  % (d[0], d[1], d[2], d[3], d[4]))
    print()
    print("%d whole-band searches, %d with a different window (%.1f%%)"
          % (tot, bad, 100.0 * bad / max(1, tot)))


if __name__ == "__main__":
    sys.exit(main())

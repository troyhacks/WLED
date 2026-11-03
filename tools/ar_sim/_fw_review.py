#!/usr/bin/env python3
"""REVIEW MEASUREMENT: the veto, with the firmware's own band and PLL.

Uses the firmware's band (40-160 Hz at 3 kHz) and the firmware's proportional
PLL (gain 0.10), which is what the current ar_tempo.h has.

The REFERENCE rate for a window is the rate the real tracker settles on when
it is given that window on its own - that is what "the tracker has found the
new music" looks like.
"""

import argparse
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402

TOL_BPM = 1.0
HOLD = 281


def make(margin, gain=0.10):
    t = P.Tracker()
    t.narrow_bpm = 2.0
    t.search_every = 4
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = 32
    t.net_margin = margin
    t.pll_gain = gain
    t.exact_kernel = True
    return t


def reference_bpm(odf):
    t = make(0.0)
    t.run(odf)
    return t.bpm if t.locked and t.bpm > 0 else None


def relock_time(t, cut, ref):
    log = t.bpm_log
    n = len(log)
    first = None
    for f in range(cut, n):
        if t.locked and abs(log[f] - ref) <= TOL_BPM:
            first = f
            break
    if first is None:
        return None
    for f in range(first, min(n, first + HOLD)):
        if not t.locked or abs(log[f] - ref) > 2.0 * TOL_BPM:
            return None
    return (first - cut) / P.RATE


class Watch(P.Tracker):
    def __init__(self):
        super().__init__()
        self.min_narrow = 1e9
        self.n_narrow = 0
        self.n_collapse = 0

    def sweep_step(self):
        if self.sweep_was_narrow and self.sweep_active:
            self.n_narrow += 1
            self.min_narrow = min(self.min_narrow, self.sweep_best_score)
        super().sweep_step()
        if self.need_full_reason == 'collapse':
            self.n_collapse += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=18)
    ap.add_argument("--pairs", type=int, default=8)
    ap.add_argument("--lo", type=float, default=40.0)
    ap.add_argument("--hi", type=float, default=160.0)
    args = ap.parse_args()

    t0 = time.time()
    items = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, args.lo, args.hi)
        if len(odf) < 1200:
            continue
        items.append((k, odf))
    print("%d windows  band %g-%g Hz  %.0f s"
          % (len(items), args.lo, args.hi, time.time() - t0), flush=True)

    refs = {}
    for (k, odf) in items:
        refs[k] = reference_bpm(odf)
    print("references done  %.0f s" % (time.time() - t0), flush=True)

    pairs = []
    for i in range(len(items)):
        bpm = refs[items[i][0]]
        if bpm is None:
            continue
        for j in range(i + 1, len(items)):
            bpm2 = refs[items[j][0]]
            if bpm2 is None or abs(bpm2 - bpm) < 6.0:
                continue
            pairs.append((items[i], items[j], bpm2))
            break
        if len(pairs) >= args.pairs:
            break
    print("%d pairs  %.0f s" % (len(pairs), time.time() - t0), flush=True)

    print()
    print("--- the collapse path (a narrow score under MIN_SCORE=%.2f) ---"
          % P.MIN_SCORE)
    w = Watch()
    w.narrow_bpm = 2.0
    w.search_every = 4
    w.edge_rule = False
    w.collapse_trigger = True
    w.full_every = 32
    w.net_margin = 0.20
    w.pll_gain = 0.10
    w.exact_kernel = True
    for (k, odf) in items:
        w.run(odf)
    print("narrow searches %d   lowest narrow score %.4f   collapses %d"
          % (w.n_narrow, w.min_narrow, w.n_collapse), flush=True)

    print()
    print("%-52s %9s %9s %8s" % ("pair", "no veto", "veto", "truth"))
    times = {(0.0, 0.10): [], (0.20, 0.10): []}
    never = {(0.0, 0.10): 0, (0.20, 0.10): 0}
    for (a, b, ref) in pairs:
        cut = len(a[1])
        cat = np.concatenate((a[1], b[1]))
        row = []
        for (margin, gain) in ((0.0, 0.10), (0.20, 0.10)):
            t = make(margin, gain)
            t.run(cat)
            r = relock_time(t, cut, ref)
            times[(margin, gain)].append(999.0 if r is None else r)
            if r is None:
                never[(margin, gain)] += 1
            row.append(r)
        print("%-52s %9s %9s %8.2f"
              % (("%s -> %s" % (a[0][:24], b[0][:24])),
                 ("%.2f s" % row[0]) if row[0] is not None else "NEVER",
                 ("%.2f s" % row[1]) if row[1] is not None else "NEVER",
                 ref), flush=True)

    print()
    for (margin, gain) in ((0.0, 0.10), (0.20, 0.10)):
        arr = np.array(times[(margin, gain)])
        print("margin %.2f gain %.2f   median %.2f s   worst %.2f s   never %d"
              % (margin, gain, float(np.median(arr)), float(arr.max()),
                 never[(margin, gain)]), flush=True)

    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""CHARACTERISE THE VETO'S TAIL - with the reviewer's own reference method.

For each track-change pair: run the tracker with the veto and log every
whole-band search after the change - the local (narrow) score, the far
score, the ratio far/local, and whether the far rate was the TRUE new
rate.

The question: when the veto holds the grid for a long time, is the far
rate GENUINELY better evidence (so the veto is holding a stale rate) or a
NEAR TIE (so the veto is doing the job it was built for)?
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402

TOL = 1.0
HOLD = 281


class Log(P.Tracker):
    def __init__(self):
        super().__init__()
        self.log = []

    def sweep_start(self):
        pre_far = self.net_far
        super().sweep_start()
        if self.sweep_was_narrow or self.sweep_narrow_score < -1e8:
            return
        far = self.sweep_far_score
        nar = self.sweep_narrow_score
        self.log.append(dict(
            frame=self.frame_index,
            grid=self.bpm,
            nar=nar, far=far,
            ratio=(far / nar) if nar > 0 else float('inf'),
            nar_bpm=self.sweep_narrow_bpm,
            far_bpm=self.sweep_far_bpm,
            took_far=(self.net_far > pre_far),
        ))


def make(margin):
    t = Log()
    t.narrow_bpm = 2.0
    t.search_every = 4
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = 32
    t.net_margin = margin
    t.pll_gain = 0.10
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
        if t.locked and abs(log[f] - ref) <= TOL:
            first = f
            break
    if first is None:
        return None
    for f in range(first, min(n, first + HOLD)):
        if not t.locked or abs(log[f] - ref) > 2.0 * TOL:
            return None
    return (first - cut) / P.RATE


def main():
    lo, hi = 80.0, 320.0
    windows = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    items = []
    for k in _cache.keys()[:windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, lo, hi)
        if len(odf) < 1200:
            continue
        items.append((k, odf))
    print("%d windows" % len(items), flush=True)

    refs = {k: reference_bpm(o) for (k, o) in items}
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
    print("%d pairs" % len(pairs), flush=True)
    print()

    print("%-44s %8s %8s %8s %8s %9s %9s" %
          ("pair", "noveto", "veto", "truth", "maxrat", "truefar", "vetoed"))
    for (a, b, ref) in pairs:
        cut = len(a[1])
        cat = np.concatenate((a[1], b[1]))
        r0 = relock_time(make(0.0), cut, ref) if False else None
        t0 = make(0.0)
        t0.run(cat)
        r0 = relock_time(t0, cut, ref)
        t1 = make(0.20)
        t1.run(cat)
        r1 = relock_time(t1, cut, ref)
        posts = [d for d in t1.log if d['frame'] >= cut]
        maxrat = max([d['ratio'] for d in posts], default=0.0)
        # the first search at which the TRUE new rate was the far candidate
        truefar = None
        for d in posts:
            if d['far_bpm'] and abs(d['far_bpm'] - ref) <= 1.5:
                truefar = (d['frame'] - cut) / P.RATE
                break
        nveto = sum(1 for d in posts if not d['took_far'])
        print("%-44s %8s %8s %8.2f %8.3f %9s %9d" %
              (("%s -> %s" % (a[0][:19], b[0][:19])),
               ("%.2f s" % r0) if r0 is not None else "NEVER",
               ("%.2f s" % r1) if r1 is not None else "NEVER",
               ref, maxrat,
               ("%.2f s" % truefar) if truefar is not None else "-",
               nveto), flush=True)


if __name__ == "__main__":
    sys.exit(main())

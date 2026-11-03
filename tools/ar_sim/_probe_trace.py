#!/usr/bin/env python3
"""THE VETO'S EXIT, using the port's own trace hook.

For each track-change pair, run the tracker with the veto ON and read the
trace of whole-band searches.  For each search after the change, record:
  - the ratio far/local (the veto's margin test is ratio >= 1.20)
  - whether the far rate WAS the true new rate
  - whether the far rate was taken

Then answer the reviewer's question exactly:
  "the next whole-band search 32 frames later faces exactly the same
   comparison"  ->  is that true, and what is the cost of it?
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


def make(margin):
    t = P.Tracker()
    t.narrow_bpm = 2.0
    t.search_every = 4
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = 32
    t.net_margin = margin
    t.pll_gain = 0.10
    t.exact_kernel = True
    t.trace = []
    return t


def reference_bpm(odf):
    t = make(0.0)
    t.run(odf)
    return t.bpm if t.locked and t.bpm > 0 else None


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

    print("%-42s %7s %7s %8s %9s %9s %9s" %
          ("pair", "novet", "veto", "truth", "maxrat", "truefar@", "n_true"))
    n_ratio_below = 0
    for (a, b, ref) in pairs:
        cut = len(a[1])
        cat = np.concatenate((a[1], b[1]))
        t0 = make(0.0)
        t0.run(cat)
        t1 = make(0.20)
        t1.run(cat)

        def relock(t):
            log = t.bpm_log
            for f in range(cut, len(log)):
                if t.locked and abs(log[f] - ref) <= TOL:
                    ok = all(t.locked and abs(log[g] - ref) <= 2 * TOL
                             for g in range(f, min(len(log), f + HOLD)))
                    if ok:
                        return (f - cut) / P.RATE
            return None
        r0 = relock(t0)
        r1 = relock(t1)
        # the trace entries are in frame order
        posts = [d for d in t1.trace if d['frame'] >= cut]
        maxrat = max([d['ratio'] for d in posts], default=0.0)
        # the searches where the TRUE new rate was the far candidate
        tf = [d for d in posts if abs(d['far_bpm'] - ref) <= 1.5]
        truefar = ((tf[0]['frame'] - cut) / P.RATE) if tf else None
        n_true = len(tf)
        if tf and max([d['ratio'] for d in tf], default=0) <= 1.20:
            n_ratio_below += 1
        print("%-42s %7s %7s %8.2f %9.3f %9s %9d" %
              (("%s -> %s" % (a[0][:18], b[0][:18])),
               ("%.2f s" % r0) if r0 is not None else "NEVER",
               ("%.2f s" % r1) if r1 is not None else "NEVER",
               ref, maxrat,
               ("%.2f s" % truefar) if truefar is not None else "-",
               n_true), flush=True)
    print()
    print("pairs where the TRUE new rate was nominated as far but its ratio"
          " never cleared 1.20: %d" % n_ratio_below)


if __name__ == "__main__":
    sys.exit(main())

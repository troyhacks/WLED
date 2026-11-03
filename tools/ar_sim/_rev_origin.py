#!/usr/bin/env python3
"""REVIEW CHECK 8, THE CIRCUMVENTION, COUNTED.

The reviewer's claim in one testable sentence: the veto refuses a far rate,
but the narrow path walks the grid onto it anyway, so the veto's evidence
rule is circumvented.

For every corpus pair this asks, precisely:

  1. How many times did the veto REFUSE a far candidate that was the NEW
     track's rate (within 1 BPM)?  Those are the refusals the narrow path
     would have to work around.
  2. Did the grid ever arrive at the new rate?  If yes, WHICH adopt brought
     it there:
        - a whole-band search the veto APPROVED  (the veto working)
        - a REFUSED whole-band search's local best (the veto's own design)
        - a NARROW adopt  (the reviewer's circumvention)
  3. If it was a narrow adopt, how many BPM had the grid already been
     dragged by narrow adopts alone before it got there?

    python tools/ar_sim/_rev_origin.py --pairs 24
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

NARROW = 2.0
EVERY = 4
NET = 32
MARGIN = 0.20
GAIN = 0.10


class Probe(P.Tracker):
    """Records every adopt with the origin of the rate it adopted."""

    def __init__(self):
        super().__init__()
        self.adopts = []           # (frame, bpm, origin) origin in
                                  # 'narrow', 'far' (veto approved), 'local'
                                  # (a refused whole-band search's local best)

    def sweep_start(self):
        self._narrow = (self.locked and not self.need_full
                        and self.narrow_bpm > 0.0)
        super().sweep_start()

    def sweep_step(self):
        before = self.bpm
        super().sweep_step()
        if self.bpm != before:
            if self._narrow:
                origin = "narrow"
            elif self.sweep_took_far:
                origin = "far"
            else:
                origin = "local"
            self.adopts.append((self.frame_index, self.bpm, origin))


def make():
    t = Probe()
    t.narrow_bpm = NARROW
    t.search_every = EVERY
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = NET
    t.net_margin = MARGIN
    t.veto_on_collapse = True
    t.pll_gain = GAIN
    t.exact_kernel = True
    return t


def reference_bpm(odf):
    t = P.Tracker()
    t.narrow_bpm = 0.0
    t.run(odf)
    return t.bpm if t.locked and t.bpm > 0 else None


def load_odfs(windows):
    cache_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "_rev_odf_cache")
    os.makedirs(cache_dir, exist_ok=True)
    items = []
    for k in _cache.keys()[:windows]:
        x = _cache.load(k)
        if x is None:
            continue
        cpath = os.path.join(cache_dir, k.replace("/", "_") + ".npy")
        if os.path.exists(cpath):
            odf = np.load(cpath)
        else:
            odf = B.firmware_odf(x, 80.0, 320.0)
            np.save(cpath, odf)
        if len(odf) < 1200:
            continue
        items.append((k, odf))
    return items


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=30)
    ap.add_argument("--pairs", type=int, default=24)
    ap.add_argument("--gap", type=float, default=8.0)
    args = ap.parse_args()

    t0 = time.time()
    items = load_odfs(args.windows)
    refs = {}
    for (k, odf) in items:
        refs[k] = reference_bpm(odf)
    pairs = []
    for i in range(len(items)):
        a = refs.get(items[i][0])
        if a is None:
            continue
        for j in range(len(items)):
            if j == i:
                continue
            b = refs.get(items[j][0])
            if b is None or abs(b - a) < args.gap:
                continue
            pairs.append((items[i], items[j], b))
            break
    pairs = pairs[:args.pairs]
    print("%d pairs   %.0f s" % (len(pairs), time.time() - t0), flush=True)
    print()

    refused_new = 0
    arrived = 0
    by_origin = {"narrow": 0, "far": 0, "local": 0, "none": 0}
    for (a, b, ref) in pairs:
        t = make()
        cut = len(a[1])
        t.run(np.concatenate((a[1], b[1])))
        log = np.array(t.bpm_log)

        # the refusals whose far candidate WAS the new rate
        ref_new = [r for r in t.trace
                   if r["frame"] >= cut and not r["took_far"]
                   and abs(r["far_bpm"] - ref) <= 1.0]
        refused_new += len(ref_new)

        # the first arrival at the new rate, and its origin
        hit = None
        for (f, bpm, origin) in t.adopts:
            if f >= cut and abs(bpm - ref) <= 1.0:
                hit = (f, bpm, origin)
                break
        if hit is not None:
            arrived += 1
            by_origin[hit[2]] += 1
        else:
            by_origin["none"] += 1

        print("%-42s grid %7.2f  new %7.2f  refusals-of-the-new-rate %3d  "
              "arrived: %s"
              % (("%s -> %s" % (a[0][:17], b[0][:17])),
                 log[cut - 1] if cut else 0.0, ref, len(ref_new),
                 ("%.2f s by %s" % ((hit[0] - cut) / P.RATE, hit[2]))
                 if hit is not None else "NO"), flush=True)

    print()
    print("the veto refused the NEW RATE %d times over %d pairs"
          % (refused_new, len(pairs)))
    print("the grid arrived at the new rate in %d of %d pairs" % (arrived,
                                                                 len(pairs)))
    print("  by a NARROW adopt (the reviewer's circumvention): %d"
          % by_origin["narrow"])
    print("  by a whole-band search the veto APPROVED: %d" % by_origin["far"])
    print("  by a refused whole-band search's local best: %d"
          % by_origin["local"])
    print("  it never arrived: %d" % by_origin["none"])
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

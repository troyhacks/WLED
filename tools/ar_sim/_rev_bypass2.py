#!/usr/bin/env python3
"""REVIEW CHECK 9, THE BYPASS, WITH THE PATH NAMED.

THE REVIEWER'S CLAIM, IN ONE SENTENCE:
    The veto refuses a far rate, but the narrow path - which has no evidence
    test and whose window is re-centred on the grid every four frames -
    walks the grid onto that refused rate anyway.

So for every REFUSED far candidate F, this asks:
    did the grid later arrive at F (within 0.5 BPM), and if so, WHICH adopt
    brought it there?

    narrow   - a narrow search.  The reviewer's circumvention.
    local    - a REFUSED whole-band search's local best.  The veto's OWN
               design: ar_tempo.h 1060-1067 adopts the best rate INSIDE the
               window when the veto refuses.
    far      - a whole-band search the veto APPROVED.  The veto working.

and how long it took, and how far the grid was from F when it got there.

    python tools/ar_sim/_rev_bypass2.py --pairs 12
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
    def __init__(self):
        super().__init__()
        self.adopts = []           # (frame, bpm, origin)

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
    ap.add_argument("--pairs", type=int, default=12)
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

    by_path = {"narrow": 0, "local": 0, "far": 0}
    times = []
    nar_travel = []
    wb_travel = []
    print("=" * 104)
    print("DID THE GRID ARRIVE AT A REFUSED FAR RATE, AND BY WHICH PATH?")
    print("=" * 104)
    for (a, b, ref) in pairs:
        t = make()
        cut = len(a[1])
        t.run(np.concatenate((a[1], b[1])))
        log = np.array(t.bpm_log)

        refused = [r for r in t.trace if r["frame"] >= cut and not r["took_far"]
                   and r["far_bpm"] > 0.0]
        # the first refused far candidate the grid later arrived at
        first = None
        for r in refused:
            F = r["far_bpm"]
            for (f, bpm, origin) in t.adopts:
                if f < r["frame"]:
                    continue
                if abs(bpm - F) <= 0.5:
                    first = (r, f, bpm, origin)
                    break
            if first is not None:
                break

        if first is None:
            print("%-42s  %d refusals, the grid reached NONE of them"
                  % (("%s -> %s" % (a[0][:17], b[0][:17])), len(refused)),
                  flush=True)
            continue
        (r, f, bpm, origin) = first
        dt = (f - r["frame"]) / P.RATE
        times.append(dt)
        by_path[origin] += 1
        print("%-42s  refused %7.2f at %6.2f s; the grid arrived at %7.2f "
              "at %6.2f s BY %s"
              % (("%s -> %s" % (a[0][:17], b[0][:17])),
                 r["far_bpm"], (r["frame"] - cut) / P.RATE, bpm,
                 (f - cut) / P.RATE, origin), flush=True)

        # the travel the narrow path contributed after the cut
        nt = 0.0
        wt = 0.0
        prev = log[cut - 1]
        for (ff, bb, oo) in t.adopts:
            if ff < cut:
                continue
            d = bb - prev
            if oo == "narrow":
                nt += d
            else:
                wt += d
            prev = bb
        nar_travel.append(nt)
        wb_travel.append(wt)

    print()
    print("the grid arrived at a refused far rate in %d pairs" % sum(by_path.values()))
    print("  by a NARROW adopt (the reviewer's circumvention): %d"
          % by_path["narrow"])
    print("  by a refused whole-band search's local best: %d"
          % by_path["local"])
    print("  by a whole-band search the veto APPROVED: %d" % by_path["far"])
    if times:
        print("  the time from the refusal to the arrival: mean %.2f s, "
              "max %.2f s" % (float(np.mean(times)), max(times)))
    if nar_travel:
        print()
        print("the NET travel after the cut, split by path:")
        print("  the narrow path's net travel: mean %+.2f BPM, max |%.2f| BPM"
              % (float(np.mean(nar_travel)),
                 max(abs(x) for x in nar_travel)))
        print("  the whole-band path's net travel: mean %+.2f BPM"
              % float(np.mean(wb_travel)))
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

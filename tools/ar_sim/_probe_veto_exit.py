#!/usr/bin/env python3
"""Does the veto ever exit?  How long can it hold the grid?

Instruments the port's veto: for each track-change pair, count the
consecutive vetoes, the longest run, and the time until the far rate is
finally ACCEPTED (the grid moves onto the new music's rate).
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402

RATE = P.RATE


class Probe(P.Tracker):
    def __init__(self):
        super().__init__()
        self.veto_frames = []      # frame index of every veto
        self.far_frames = []       # frame index of every far win that was refused

    def sweep_start(self):
        self._pre_far = self.net_far
        self._pre_veto = self.net_vetoes
        super().sweep_start()

    def sweep_step(self):
        super().sweep_step()
        if self.net_vetoes > self._pre_veto:
            self.veto_frames.append(self.frame_index)
        if self.net_far > self._pre_far and self.net_vetoes == self._pre_veto:
            self.far_frames.append(self.frame_index)


def main():
    lo, hi = 80.0, 320.0
    windows = int(sys.argv[1]) if len(sys.argv) > 1 else 18
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

    def ref_bpm(odf):
        t = P.Tracker()
        t.narrow_bpm = 0.0
        t.run(odf)
        return t.bpm if t.locked and t.bpm > 0 else None

    refs = {k: ref_bpm(o) for (k, o) in items}
    pairs = []
    for i in range(len(items)):
        b = refs[items[i][0]]
        if b is None:
            continue
        for j in range(i + 1, len(items)):
            b2 = refs[items[j][0]]
            if b2 is None or abs(b2 - b) < 6.0:
                continue
            pairs.append((items[i], items[j], b2))
            break
    print("%d pairs" % len(pairs), flush=True)

    print()
    print("%-40s %6s %8s %9s %9s %9s" %
          ("pair", "vetoes", "run", "1st veto", "far ok", "accept"))
    worst_run = 0
    worst_hold = 0.0
    for (a, b, ref) in pairs:
        t = Probe()
        t.narrow_bpm = 2.0
        t.search_every = 4
        t.edge_rule = False
        t.collapse_trigger = True
        t.full_every = 32
        t.net_margin = 0.20
        t.pll_gain = 0.10
        t.exact_kernel = True
        cut = len(a[1])
        t.run(np.concatenate((a[1], b[1])))
        # the longest consecutive run of vetoes, and the longest gap in
        # seconds between two vetoes (the grid was held that long)
        vf = t.veto_frames
        run = 0
        best_run = 0
        best_hold = 0.0
        for i, f in enumerate(vf):
            if i and f - vf[i - 1] <= 40:
                run += 1
            else:
                run = 1
            best_run = max(best_run, run)
            if i:
                best_hold = max(best_hold, (f - vf[i - 1]) / RATE)
        worst_run = max(worst_run, best_run)
        worst_hold = max(worst_hold, best_hold)
        # when is the far rate finally ACCEPTED?  The bpm log settles on ref.
        log = np.array(t.bpm_log)
        acc = None
        for f in range(cut, len(log)):
            if t.locked and abs(log[f] - ref) <= 1.0:
                acc = (f - cut) / RATE
                break
        first_veto = ((vf[0] - cut) / RATE) if vf else None
        print("%-40s %6d %8d %9s %9s %9s" %
              (("%s -> %s" % (a[0][:18], b[0][:18])),
               len(vf), best_run,
               "%.2f s" % first_veto if first_veto is not None else "-",
               "%.2f s" % ((t.far_frames[0] - cut) / RATE) if t.far_frames else "-",
               "%.2f s" % acc if acc is not None else "NEVER"),
              flush=True)
    print()
    print("longest consecutive-veto run %d   longest hold between vetoes %.2f s"
          % (worst_run, worst_hold))


if __name__ == "__main__":
    sys.exit(main())

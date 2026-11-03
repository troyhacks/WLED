#!/usr/bin/env python3
"""CHARACTERISE THE VETO'S TAIL.

For each track-change pair, run the tracker with the veto and log every
whole-band search: the local (narrow) score, the far score, the ratio
far/local, and which side won.

The question: when the veto holds the grid on the old rate for a long time,
is the far rate GENUINELY better evidence (so the veto is holding a stale
rate - a defect) or is it a NEAR TIE (so the veto is doing exactly the job
it was built for, and the long hold is the price of that job)?
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402

RATE = P.RATE


class Log(P.Tracker):
    def __init__(self):
        super().__init__()
        self.log = []

    def sweep_start(self):
        super().sweep_start()
        if self.sweep_was_narrow:
            return
        if self.sweep_narrow_score < -1e8:
            return
        far = self.sweep_far_score
        nar = self.sweep_narrow_score
        ratio = (far / nar) if nar > 0 else float('inf')
        self.log.append(dict(
            frame=self.frame_index,
            grid=self.bpm,
            nar=nar, far=far, ratio=ratio,
            nar_bpm=self.sweep_narrow_bpm, far_bpm=self.sweep_far_bpm,
            best=self.sweep_best_bpm,
            took_far=(self.net_far > self._pf),
        ))
        self._pf = self.net_far

    def _pf_init(self):
        self._pf = 0


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
    t._pf = 0
    return t


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

    print("%-46s %7s %8s %8s %8s %8s %9s" %
          ("pair", "relock", "maxrat", "farok", "grid@ok", "farbpm", "hold"))
    for (a, b, ref) in pairs:
        t = make(0.20)
        cut = len(a[1])
        t.run(np.concatenate((a[1], b[1])))
        lg = t.bpm_log
        relock = None
        for f in range(cut, len(lg)):
            if t.locked and abs(lg[f] - ref) <= 1.0:
                relock = (f - cut) / RATE
                break
        # the whole-band searches after the change
        posts = [d for d in t.log if d['frame'] >= cut]
        # the first search where the TRUE new rate was the far candidate
        farok = None
        gridat = None
        maxrat = 0.0
        for d in posts:
            if d['far_bpm'] and abs(d['far_bpm'] - ref) <= 1.5:
                if farok is None:
                    farok = (d['frame'] - cut) / RATE
                    gridat = d['grid']
            if d['nar'] > 0:
                maxrat = max(maxrat, d['ratio'])
        # how long did the grid sit on the OLD rate after the change?
        old = refs[a[0]]
        hold = None
        for f in range(cut, len(lg)):
            if not (t.locked and abs(lg[f] - ref) <= 1.0):
                hold = (f - cut) / RATE
            else:
                break
        print("%-46s %7s %8.3f %8s %8s %8.2f %9s" %
              (("%s -> %s" % (a[0][:20], b[0][:20])),
               ("%.2f s" % relock) if relock is not None else "NEVER",
               maxrat,
               ("%.2f s" % farok) if farok is not None else "-",
               ("%.2f" % gridat) if gridat is not None else "-",
               ref,
               ("%.2f s" % hold) if hold is not None else "-"),
              flush=True)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""THE STUCK TEST.

The reviewer's claim, made concrete:

  "After a veto the grid has not moved, needFullSearch is false, and the
   narrow search keeps scoring above MIN_SCORE (the collapse never fires),
   so the next whole-band search 32 frames later faces exactly the same
   comparison."

Decisive measurement, per track-change pair, with the veto ON:

  oldrate  = the rate the grid was on before the change
  truth    = the rate the new track settles on when tracked alone
  stuck    = how long the PUBLISHED bpm stays within 3 BPM of oldrate
             AFTER the change (i.e. the grid is still serving the old track)
  cleared  = the first time the published bpm is within 3 BPM of truth and
             STAYS there for 3 s
  maxrat   = the largest far/narrow ratio seen in any whole-band search
             after the change - if this never exceeds 1.20 the veto has
             NO WAY to let the far rate through

If maxrat never clears 1.20 and stuck is the whole track, the grid serves
the OLD track's rate forever: that is the failure the reviewer describes.
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
        super().sweep_start()
        if self.sweep_was_narrow or self.sweep_narrow_score < -1e8:
            return
        nar = self.sweep_narrow_score
        far = self.sweep_far_score
        self.log.append(dict(
            frame=self.frame_index,
            nar=nar, far=far,
            ratio=(far / nar) if nar > 0 else float('inf'),
            far_bpm=self.sweep_far_bpm,
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

    print("%-44s %8s %8s %9s %9s %8s %8s" %
          ("pair", "old", "truth", "stuck", "cleared", "maxrat", "track"))
    worst_stuck = 0.0
    n_never_cleared = 0
    n_maxrat_low = 0
    for (a, b, ref) in pairs:
        cut = len(a[1])
        cat = np.concatenate((a[1], b[1]))
        t = make(0.20)
        t.run(cat)
        log = np.array(t.bpm_log)
        track_s = (len(cat) - cut) / P.RATE
        old = refs[a[0]]
        # stuck: the last frame at which the published bpm was still within
        # 3 BPM of the OLD rate
        stuck = 0.0
        for f in range(cut, len(log)):
            if log[f] > 0 and abs(log[f] - old) <= 3.0:
                stuck = (f - cut) / P.RATE
            else:
                break
        # cleared: the first frame within 1 BPM of truth that holds 3 s
        cleared = None
        for f in range(cut, len(log)):
            if t.locked and abs(log[f] - ref) <= TOL:
                ok = True
                for g in range(f, min(len(log), f + HOLD)):
                    if not t.locked or abs(log[g] - ref) > 2.0 * TOL:
                        ok = False
                        break
                if ok:
                    cleared = (f - cut) / P.RATE
                break
        posts = [d for d in t.log if d['frame'] >= cut]
        maxrat = max([d['ratio'] for d in posts], default=0.0)
        worst_stuck = max(worst_stuck, stuck)
        if cleared is None:
            n_never_cleared += 1
        if maxrat <= 1.20:
            n_maxrat_low += 1
        print("%-44s %8.2f %8.2f %9s %9s %8.3f %8.1f" %
              (("%s -> %s" % (a[0][:19], b[0][:19])), old, ref,
               ("%.2f s" % stuck) if stuck > 0 else "-",
               ("%.2f s" % cleared) if cleared is not None else "NEVER",
               maxrat, track_s), flush=True)
    print()
    print("worst stuck %.2f s   never cleared %d   maxrat never > 1.20: %d"
          % (worst_stuck, n_never_cleared, n_maxrat_low))


if __name__ == "__main__":
    sys.exit(main())

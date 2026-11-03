#!/usr/bin/env python3
"""DOES THE CODE HAVE A WORKING EXIT?

The current ar_tempo.h veto block has ONE code-level escape:

    const bool localAlive = arTempo.sweepNarrowScore >= AR_TEMPO_MIN_SCORE;
    if (!localAlive) takeFar = farAlive;   // the local rate is gone

The reviewer's claim is that this escape never fires: the narrow score
never falls under MIN_SCORE, so the ONLY thing that can ever let the far
rate through is the comb's 12 s window turning over - a property of the
music, not of the code.

Measure, over every whole-band search on the corpus:
  - the MINIMUM sweep_narrow_score (the escape's trigger)
  - how often localAlive is false (the escape firing)
  - how often the far candidate is taken
  - and how many pairs end with the grid STILL ON THE OLD RATE at the end
    of the track, with no code path able to release it
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402


class Log(P.Tracker):
    def __init__(self):
        super().__init__()
        self.min_nar = 1e9
        self.n_full = 0
        self.n_escape = 0        # localAlive false -> the far wins
        self.n_far_taken = 0
        self.n_veto = 0
        self.min_nar_while_locked = 1e9

    def sweep_start(self):
        pre_far = self.net_far
        pre_veto = self.net_vetoes
        super().sweep_start()
        if self.sweep_was_narrow or self.sweep_narrow_score < -1e8:
            return
        self.n_full += 1
        nar = self.sweep_narrow_score
        self.min_nar = min(self.min_nar, nar)
        if nar < P.MIN_SCORE:
            self.n_escape += 1
        if self.net_far > pre_far:
            self.n_far_taken += 1
        if self.net_vetoes > pre_veto:
            self.n_veto += 1


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

    w = make(0.20)
    for (k, odf) in items:
        w.run(odf)
    print()
    print("whole-band searches %d" % w.n_full)
    print("MINIMUM narrow score on a whole-band search   %.4f" % w.min_nar)
    print("MIN_SCORE (the escape's trigger)               %.4f" % P.MIN_SCORE)
    print("the escape fired (localAlive false) %d times" % w.n_escape)
    print("far taken %d   vetoed %d" % (w.n_far_taken, w.n_veto))

    # the pairs, and the end state
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
    print()
    print("%d pairs" % len(pairs))
    print()
    print("%-44s %8s %8s %8s %8s %8s" %
          ("pair", "old", "truth", "end", "escapes", "stuck"))
    n_stuck = 0
    for (a, b, ref) in pairs:
        cut = len(a[1])
        cat = np.concatenate((a[1], b[1]))
        t = make(0.20)
        t.run(cat)
        log = np.array(t.bpm_log)
        old = refs[a[0]]
        end = log[-1] if len(log) else 0.0
        stuck = end > 0 and abs(end - old) <= 3.0
        if stuck:
            n_stuck += 1
        print("%-44s %8.2f %8.2f %8.2f %8d %8s" %
              (("%s -> %s" % (a[0][:19], b[0][:19])), old, ref, end,
               t.n_escape, "YES" if stuck else "-"), flush=True)
    print()
    print("pairs still serving the OLD rate at the end: %d of %d"
          % (n_stuck, len(pairs)))


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Does the value-winner choice flip the veto's verdict?

Two rules for the veto's FAR side:
  A (firmware): the whole-band value winner (score - prior)
  B (proposed): the best RAW-score candidate outside the narrow window

Count how often the verdict differs, and how much raw evidence the
firmware's rule throws away in each direction.
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402


class Flipped(P.Tracker):
    def __init__(self):
        super().__init__()
        self.raw = None
        self.judged = 0
        self.flip = 0
        self.refuse_a_grant_b = 0     # A refuses, B grants (missed change)
        self.grant_a_refuse_b = 0     # A grants, B refuses (conservative)
        self.missed = []              # raw score of the candidate A ignored
        self.same_bpm = 0             # the two rules pick the SAME rate

    def comb_scores_all(self, frames, strengths, bpms):
        s, p = super().comb_scores_all(frames, strengths, bpms)
        self.raw = (np.array(bpms, dtype=float), np.array(s, dtype=float))
        return s, p

    def sweep_start(self):
        self.raw = None
        super().sweep_start()
        if self.raw is None or self.sweep_was_narrow or not self.sweep_active:
            return
        bpms, scores = self.raw
        if len(bpms) != P.CAND_COUNT:
            return
        values = scores - self.prior
        jv = int(np.argmax(values))
        jr = int(np.argmax(scores))
        f, c = P.narrow_window(self.bpm, self.narrow_bpm)
        if f < 0 or c <= 0:
            return
        jn = int(np.arange(f, f + c)[np.argmax(scores[f:f + c])])
        if f <= jv <= f + c - 1:
            return                      # A: the far side is inside -> no veto
        self.judged += 1
        alive = float(scores[jn]) >= P.MIN_SCORE
        # rule A: the value winner is the far side
        a_refuses = alive and float(scores[jv]) < float(scores[jn]) * (1.0 + self.net_margin)
        # rule B: the best raw candidate OUTSIDE the window is the far side
        mask = np.ones(len(scores), dtype=bool)
        mask[f:f + c] = False
        outside = np.where(mask)[0]
        jb = int(outside[np.argmax(scores[outside])])
        b_refuses = alive and float(scores[jb]) < float(scores[jn]) * (1.0 + self.net_margin)
        if a_refuses != b_refuses:
            self.flip += 1
            if a_refuses:
                # A refuses the change, B would grant it: A ignored the
                # best-evidence candidate outside the window.
                self.refuse_a_grant_b += 1
                self.missed.append(float(scores[jb] - scores[jv]))
            else:
                self.grant_a_refuse_b += 1
        if jv == jb:
            self.same_bpm += 1


def main():
    items = []
    for k in _cache.keys()[:18]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, 40.0, 160.0)
        if len(odf) < 1200:
            continue
        items.append((k, odf))
    t = Flipped()
    t.narrow_bpm = 2.0
    t.search_every = 4
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = 32
    t.net_margin = 0.20
    t.pll_gain = 0.10
    t.exact_kernel = True
    for (k, odf) in items:
        t.run(odf)
    print("searches the veto judges %d" % t.judged)
    print("verdict flips %d (%.0f %%)" % (t.flip, 100.0 * t.flip / max(1, t.judged)))
    print("  A refuses, B grants (a real change A threw away): %d" % t.refuse_a_grant_b)
    print("  A grants, B refuses (A is more permissive): %d" % t.grant_a_refuse_b)
    m = np.array(t.missed) if t.missed else np.zeros(1)
    print("  raw score A ignored when it refused a change: mean %.4f max %.4f"
          % (float(m.mean()), float(m.max())))
    print("the two rules pick the same rate in %d of %d judged searches"
          % (t.same_bpm, t.judged))


if __name__ == "__main__":
    sys.exit(main())

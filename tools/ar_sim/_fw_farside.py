#!/usr/bin/env python3
"""WHICH CANDIDATE DOES THE VETO JUDGE?

The veto compares two numbers:
  - the FAR candidate:  the whole-band winner, chosen by VALUE (score - prior)
  - the NEAR candidate: the best in the narrow window, chosen by RAW score

The comment says the prior is left out of the fight.  It is left out of the
COMPARISON - but it decides WHICH far candidate is compared.  The prior costs
up to 0.55 of score at the band edges, so the far candidate is the one the
prior likes, not necessarily the one with the best evidence.

This counts how often the two differ, and how much score the choice throws
away.
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402


class Counted(P.Tracker):
    def __init__(self):
        super().__init__()
        self.full = 0
        self.differ = 0
        self.lost = []
        self.outside_raw = 0
        self.outside_value = 0
        self.judged = 0
        self.judged_differ = 0
        self.judged_lost = []

    def comb_scores_all(self, frames, strengths, bpms):
        s, p = super().comb_scores_all(frames, strengths, bpms)
        self._raw = (np.array(bpms, dtype=float), np.array(s, dtype=float))
        return s, p

    def sweep_start(self):
        self._raw = None
        super().sweep_start()
        if self._raw is None or self.sweep_was_narrow or not self.sweep_active:
            return
        bpms, scores = self._raw
        if len(bpms) != P.CAND_COUNT:
            return
        values = scores - self.prior
        jv = int(np.argmax(values))
        jr = int(np.argmax(scores))
        self.full += 1
        if jv != jr:
            self.differ += 1
            self.lost.append(float(scores[jr] - scores[jv]))
        # and: where does the best RAW score sit relative to the grid's
        # narrow window?
        f, c = P.narrow_window(self.bpm, self.narrow_bpm)
        if not (f <= jr <= f + c - 1):
            self.outside_raw += 1
        if not (f <= jv <= f + c - 1):
            self.outside_value += 1
            self.judged += 1
            if jv != jr:
                self.judged_differ += 1
                self.judged_lost.append(float(scores[jr] - scores[jv]))


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
    t = Counted()
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
    lost = np.array(t.lost) if t.lost else np.zeros(1)
    print("whole-band searches %d" % t.full)
    print("the value winner is not the raw winner in %d of them (%.0f %%)"
          % (t.differ, 100.0 * t.differ / max(1, t.full)))
    print("the raw score thrown away by that choice: mean %.4f  max %.4f"
          % (float(lost.mean()), float(lost.max())))
    print("the best raw candidate is OUTSIDE the narrow window in %d searches"
          % t.outside_raw)
    print("the value winner is OUTSIDE the narrow window in %d searches"
          % t.outside_value)
    jl = np.array(t.judged_lost) if t.judged_lost else np.zeros(1)
    print("of those, the value winner is not the raw winner in %d (%.0f %%)"
          % (t.judged_differ, 100.0*t.judged_differ/max(1,t.judged)))
    print("  raw score thrown away there: mean %.4f  max %.4f"
          % (float(jl.mean()), float(jl.max())))


if __name__ == "__main__":
    sys.exit(main())

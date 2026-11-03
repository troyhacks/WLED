#!/usr/bin/env python3
"""IS THE REVIEWER'S PROPOSED FIX COHERENT?

The fix: apply the veto's evidence rule to the NARROW path as well - the
narrow winner may only move the grid if its raw score beats the rate the
grid is on by (1 + AR_TEMPO_NET_MARGIN) = 1.70.

For that rule to be usable on the narrow path, the best candidate in a
+/-2 BPM window must usually be MUCH better than the rate the grid is
currently on.  This measures the actual ratio distribution.

If the ratio sits near 1.0 - which it will, because the narrow window is
re-centred on the grid and the grid is usually already on the peak - then
a 1.70x rule can never fire and the narrow path is frozen forever.
"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache, _fw_band as B, _fw_port as P

HERE = os.path.dirname(os.path.abspath(__file__))
cd = os.path.join(HERE, "_rev_odf_cache")


class Probe(P.Tracker):
    def comb_scores_all(self, frames, strengths, bpms):
        s, p = super().comb_scores_all(frames, strengths, bpms)
        self._scores = s
        self._bpms = bpms
        return s, p

    def sweep_start(self):
        self._scores = None
        super().sweep_start()
        if not self.sweep_was_narrow or self._scores is None:
            return
        if len(self._scores) < 2:
            return
        # the grid's own rate, as the nearest candidate in the window
        j0 = int(np.argmin(np.abs(self._bpms - self.bpm)))
        own = float(self._scores[j0])
        best = float(self._scores.max())
        if own <= 0.0:
            return
        # the edge candidates: how far from the peak is the winner?
        jb = int(np.argmax(self._scores))
        self.ratios.append(best / own)
        self.edges.append(abs(jb - j0))


def load(windows):
    items = []
    for k in _cache.keys()[:windows]:
        x = _cache.load(k)
        if x is None:
            continue
        cp = os.path.join(cd, k.replace("/", "_") + ".npy")
        if os.path.exists(cp):
            odf = np.load(cp)
        else:
            odf = B.firmware_odf(x, 80.0, 320.0)
            np.save(cp, odf)
        if len(odf) >= 1200:
            items.append((k, odf))
    return items


def main():
    items = load(30)
    ratios = []
    edges = []
    for (k, odf) in items:
        t = Probe()
        t.narrow_bpm = 2.0
        t.search_every = 4
        t.edge_rule = False
        t.collapse_trigger = True
        t.full_every = 32
        t.net_margin = 0.70
        t.veto_on_collapse = True
        t.pll_gain = 0.10
        t.exact_kernel = True
        t.ratios = ratios
        t.edges = edges
        t.run(odf)
    r = np.array(ratios)
    e = np.array(edges)
    print("narrow searches measured: %d" % len(r))
    if len(r):
        print("  best-in-window raw score / the grid's own rate raw score")
        print("    median %.4f   mean %.4f   p90 %.4f   max %.4f"
              % (np.median(r), r.mean(), np.percentile(r, 90), r.max()))
        for thr in (1.20, 1.35, 1.50, 1.70, 2.00):
            print("    windows where the best is >= %.2fx the grid's own: %4d  (%.1f%%)"
                  % (thr, (r >= thr).sum(), 100.0 * (r >= thr).mean()))
    if len(e):
        print("  the winner's distance from the grid's own candidate, in steps")
        print("    median %.1f   mean %.1f   max %.1f  (window half-width is 8)"
              % (np.median(e), e.mean(), e.max()))
        print("    the winner was AT THE EDGE (>=7 steps out): %d of %d  (%.1f%%)"
              % ((e >= 7).sum(), len(e), 100.0 * (e >= 7).mean()))


main()

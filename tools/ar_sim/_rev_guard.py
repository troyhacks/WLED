#!/usr/bin/env python3
"""A GUARD ON THE NARROW PATH, WITH A MARGIN THAT FITS A 2 BPM WINDOW.

The veto's margin (0.70) cannot be transplanted: inside a +/-2 BPM window
the best candidate's raw score is at most 1.33x the grid's own rate's
score (median 1.0000), and 0 of 20306 windows ever reach 1.70x, so the
veto's rule would never fire there.

But a SMALL margin fits.  This measures: refuse to adopt a narrow winner
whose raw score is below the score at the grid's own rate times (1 + m),
for m in 0.02 .. 0.20.
"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache, _fw_band as B, _fw_port as P
HERE = os.path.dirname(os.path.abspath(__file__))
cd = os.path.join(HERE, "_rev_odf_cache")

class Probe(P.Tracker):
    """Adds the narrow-path guard."""
    nmargin = None            # None = no guard
    def comb_scores_all(self, frames, strengths, bpms):
        s, p = super().comb_scores_all(frames, strengths, bpms)
        self._sc = s; self._bp = bpms
        return s, p
    def sweep_start(self):
        self._sc = None
        self._narrow = (self.locked and not self.need_full and self.narrow_bpm > 0.0)
        super().sweep_start()
        if self.nmargin is None or not self._narrow or self._sc is None:
            return
        if len(self._sc) < 2: return
        j0 = int(np.argmin(np.abs(self._bp - self.bpm)))
        own = float(self._sc[j0])
        jb = int(np.argmax(self._sc))
        if own <= 0.0: return
        if float(self._sc[jb]) < own * (1.0 + self.nmargin):
            # refuse the winner: keep the grid where it is
            self.sweep_best_bpm = self.bpm
            self.sweep_best_phase = self.sweep_best_phase
            self.sweep_best_score = own

def load(n):
    out = []
    for k in _cache.keys()[:n]:
        x = _cache.load(k)
        if x is None: continue
        cp = os.path.join(cd, k.replace("/", "_") + ".npy")
        odf = np.load(cp) if os.path.exists(cp) else B.firmware_odf(x, 80.0, 320.0)
        if not os.path.exists(cp): np.save(cp, odf)
        if len(odf) >= 1200: out.append((k, odf))
    return out

def ref_bpm(odf):
    t = P.Tracker(); t.narrow_bpm = 0.0; t.run(odf)
    return t.bpm if t.locked and t.bpm > 0 else None

items = load(30)
refs = {k: ref_bpm(o) for (k, o) in items}
pairs = []
for i in range(len(items)):
    a = refs.get(items[i][0])
    if a is None: continue
    for j in range(len(items)):
        if j == i: continue
        b = refs.get(items[j][0])
        if b is None or abs(b - a) < 8.0: continue
        pairs.append((items[i], items[j], b)); break
pairs = pairs[:14]

def run(m):
    reached = 0; errs = []; bad = 0; osc = 0
    for (a, b, r) in pairs:
        t = Probe(); t.nmargin = m
        t.narrow_bpm = 2.0; t.search_every = 4; t.edge_rule = False
        t.collapse_trigger = True; t.full_every = 32
        t.net_margin = 0.70; t.veto_on_collapse = True; t.exact_kernel = True
        cut = len(a[1])
        t.run(np.concatenate((a[1], b[1])))
        log = np.array(t.bpm_log)[cut:]
        if np.any(np.abs(log - r) <= 1.0): reached += 1
        e = abs(log[-1] - r); errs.append(e)
        if e > 5.0: bad += 1
        # hysteresis flips
        side = 0; flips = 0
        for v in log:
            if v > r + 0.5: s = 1
            elif v < r - 0.5: s = -1
            else: continue
            if side != 0 and s != side: flips += 1
            side = s
        if flips >= 2: osc += 1
    return reached, float(np.mean(errs)), bad, osc

for m in (None, 0.02, 0.05, 0.10, 0.20):
    r, e, bd, o = run(m)
    print("narrow guard margin %-6s  reached-new %2d/14  mean |final-new| %6.2f  "
          "ended >5 BPM off %2d/14  oscillating %2d/14"
          % ("none" if m is None else "%.2f" % m, r, e, bd, o))

#!/usr/bin/env python3
"""NARROW-ONLY ADOPT STEPS, at the shipped gain 0.20."""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache, _fw_band as B, _fw_port as P
HERE = os.path.dirname(os.path.abspath(__file__))
cd = os.path.join(HERE, "_rev_odf_cache")

class Probe(P.Tracker):
    def sweep_start(self):
        self._narrow = (self.locked and not self.need_full and self.narrow_bpm > 0.0)
        super().sweep_start()
    def sweep_step(self):
        before = self.bpm
        super().sweep_step()
        if self.bpm != before:
            self._adopts.append((self.frame_index, before, self.bpm, self._narrow))

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

items = load(30)
nsteps = []; wsteps = []
adopt_times = []
for (k, odf) in items:
    t = Probe(); t._adopts = []
    t.narrow_bpm = 2.0; t.search_every = 4; t.edge_rule = False
    t.collapse_trigger = True; t.full_every = 32
    t.net_margin = 0.70; t.veto_on_collapse = True; t.exact_kernel = True
    t.run(odf)
    for (f, old, new, nw) in t._adopts:
        (nsteps if nw else wsteps).append(abs(new - old))
n = np.array(nsteps); w = np.array(wsteps)
print("NARROW adopts: n %d  median %.4f  mean %.4f  p99 %.4f  MAX %.4f BPM"
      % (len(n), np.median(n), n.mean(), np.percentile(n, 99), n.max()))
print("  exceeded the reviewer's 0.2 BPM per-adopt ceiling: %d of %d (%.1f%%)"
      % ((n > 0.2).sum(), len(n), 100.0*(n > 0.2).mean()))
print("WHOLE-BAND adopts: n %d  median %.4f  mean %.4f  MAX %.4f BPM"
      % (len(w), np.median(w), w.mean(), w.max()))
print()
print("the reviewer's claimed sustained rate: 0.2 BPM x 23.4 adopts/s = 4.7 BPM/s")
print("the measured MEAN narrow step is %.4f BPM; the narrow search runs every 4"
      % n.mean())
print("frames = %.1f adopts/s; so the mean narrow travel rate is %.3f BPM/s"
      % (P.RATE/4.0, n.mean() * P.RATE / 4.0))

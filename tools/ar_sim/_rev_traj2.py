#!/usr/bin/env python3
"""THE TRAJECTORY: where does the grid go, when, and by which path.

For the pairs whose grid crosses the whole gap, print the grid every
quarter second with the narrow window bounds, so the walk is visible.
"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache, _fw_band as B, _fw_port as P

EVERY, NET, GAIN = 4, 32, 0.10

class Probe(P.Tracker):
    def __init__(self):
        super().__init__(); self.adopts = []; self.wins = []
    def sweep_start(self):
        self._narrow = (self.locked and not self.need_full and self.narrow_bpm > 0.0)
        super().sweep_start()
    def sweep_step(self):
        before = self.bpm
        super().sweep_step()
        if self.bpm != before:
            o = "narrow" if self._narrow else ("far" if self.sweep_took_far else "local")
            self.adopts.append((self.frame_index, self.bpm, o))
        if self._narrow and self.sweep_best_bpm > 0:
            self.wins.append((self.frame_index, self.sweep_best_bpm))

def make(nb, margin):
    t = Probe()
    t.narrow_bpm = nb; t.search_every = EVERY; t.edge_rule = False
    t.collapse_trigger = True; t.full_every = NET
    t.net_margin = margin; t.veto_on_collapse = True
    t.pll_gain = GAIN; t.exact_kernel = True
    return t

def reference_bpm(odf):
    t = P.Tracker(); t.narrow_bpm = 0.0; t.run(odf)
    return t.bpm if t.locked and t.bpm > 0 else None

cd = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_rev_odf_cache")
items = []
for k in _cache.keys()[:30]:
    x = _cache.load(k)
    if x is None: continue
    cp = os.path.join(cd, k.replace("/", "_") + ".npy")
    if os.path.exists(cp): odf = np.load(cp)
    else: odf = B.firmware_odf(x, 80.0, 320.0); np.save(cp, odf)
    if len(odf) >= 1200: items.append((k, odf))
refs = {k: reference_bpm(o) for (k, o) in items}
pairs = []
for i in range(len(items)):
    ra = refs.get(items[i][0])
    if ra is None: continue
    for j in range(len(items)):
        if j == i: continue
        rb = refs.get(items[j][0])
        if rb is None or abs(rb - ra) < 8.0: continue
        pairs.append((items[i], items[j], rb)); break
pairs = pairs[:14]

for idx in (0, 9):
    x, y, ref = pairs[idx]
    t = make(2.0, 0.70); cut = len(x[1])
    t.run(np.concatenate((x[1], y[1])))
    log = np.array(t.bpm_log)
    print("=== PAIR %s -> %s   new-track reference %.2f   grid at the cut %.2f"
          % (x[0][:22], y[0][:22], ref, log[cut-1]))
    wins = {}
    for (f, b) in t.wins:
        wins[f] = b
    for s in [i/4.0 for i in range(0, 60)]:
        f = cut + int(s*P.RATE)
        if f >= len(log): break
        w = wins.get(f)
        ws = ("window best %8.3f" % w) if w else ""
        if s <= 16 or s % 2 == 0:
            print("   %6.2f s  grid %8.3f   %s" % (s, log[f], ws))
    # the first time the grid is within 1 BPM of the truth
    idxs = np.where(np.abs(log[cut:] - ref) <= 1.0)[0]
    print("   first within 1 BPM of the truth: %s"
          % (("%.2f s" % (idxs[0]/P.RATE)) if len(idxs) else "NEVER"))
    # path of the adopts in the crossing phase
    cross = [a for a in t.adopts if a[0] >= cut and abs(a[1]-ref) <= 1.5]
    if cross:
        f0 = cross[0][0]
        paths = {}
        for (f, b, o) in t.adopts:
            if f0 - 200 <= f < f0 + 10: paths[o] = paths.get(o, 0) + 1
        print("   paths in the 2 s before the first arrival: %s" % paths)
    print()

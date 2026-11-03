#!/usr/bin/env python3
"""DOES THE COLLAPSE GUARD EVER FIRE AFTER A CUT?

The collapse path is the ONE guard that could put the veto in front of
the narrow path's walk: if the narrow search's score dies, the next search
is a whole-band one, and the veto governs it.

The header says the score does NOT collapse after a cut (floor 0.34-0.40
against 0.36 for steady music), so this asks whether the guard ever fires.
"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache, _fw_band as B, _fw_port as P
HERE = os.path.dirname(os.path.abspath(__file__))
cd = os.path.join(HERE, "_rev_odf_cache")

class Probe(P.Tracker):
    def __init__(self):
        super().__init__(); self.reasons = []
    def sweep_start(self):
        self.reasons.append((self.frame_index, self.need_full_reason,
                              "collapse" if self.need_full else "-"))
        super().sweep_start()

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

tot = 0; col = 0; per = []
for (a, b, r) in pairs:
    t = Probe()
    t.narrow_bpm = 2.0; t.search_every = 4; t.edge_rule = False
    t.collapse_trigger = True; t.full_every = 32
    t.net_margin = 0.70; t.veto_on_collapse = True; t.exact_kernel = True
    cut = len(a[1])
    t.run(np.concatenate((a[1], b[1])))
    after = [(f, why) for (f, why, iscol) in t.reasons if f >= cut]
    c = sum(1 for (f, why) in after if why == 'collapse')
    tot += len(after); col += c
    per.append(c)
print("whole-band searches after the cut: %d" % tot)
print("of those, COLLAPSE searches (the narrow score died): %d (%.1f%%)"
      % (col, 100.0*col/max(tot,1)))
print("per pair: %s" % per)
print()
print("the veto can only ever be reached by the narrow path's walk if the")
print("collapse fires; at %.1f%% it is not a guard on the walk." % (100.0*col/max(tot,1)))

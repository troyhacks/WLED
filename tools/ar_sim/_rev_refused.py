#!/usr/bin/env python3
"""DID THE GRID END UP AT A RATE THE VETO REFUSED?

The claim under test: the narrow path walks the grid to a rate the veto
refused, so the veto's evidence rule is bypassed.

For every pair this lists the rates the veto REFUSED as far candidates
(the whole-band search's best outside the narrow window) and whether the
grid ended at one of them.
"""
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

at_refused = 0
for (a, b, r) in pairs:
    t = Probe(); t._adopts = []
    t.narrow_bpm = 2.0; t.search_every = 4; t.edge_rule = False
    t.collapse_trigger = True; t.full_every = 32
    t.net_margin = 0.70; t.veto_on_collapse = True; t.exact_kernel = True
    cut = len(a[1])
    t.run(np.concatenate((a[1], b[1])))
    refused = [tr["far_bpm"] for tr in t.trace
               if tr["frame"] >= cut and not tr["took_far"] and tr["far_bpm"] > 0]
    end = t.bpm_log[-1]
    hit = [x for x in refused if abs(x - end) <= 0.5]
    # which path brought the grid to the end value
    path = "?"
    for (f, old, new, nw) in t._adopts:
        if abs(new - end) <= 0.01:
            path = "narrow" if nw else ("far" if t.sweep_took_far else "local")
            break
    print("%-40s truth %7.2f  end %7.2f  refused %4d rates; the end value is "
          "%s a REFUSED rate   (%s)"
          % ("%s -> %s" % (a[0][:16], b[0][:16]), r, end, len(refused),
             ("ONE of them" if hit else "NOT one of"), path))
    if hit:
        at_refused += 1
print()
print("the grid ENDED at a rate the veto had REFUSED: %d of %d pairs" % (at_refused, len(pairs)))

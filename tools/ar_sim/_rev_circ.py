#!/usr/bin/env python3
"""THE CIRCUMVENTION, COUNTED, AT BOTH MARGINS.

For every REFUSED far candidate F: did the grid later arrive at F (within
0.5 BPM), and by which path?
    narrow - the unchecked narrow path (the reviewer's circumvention)
    local  - a refused whole-band search's local best (the veto's own design)
    far    - a whole-band search the veto APPROVED
Also: is F the FOUR-THIRDS harmonic of the grid's old rate?  That is the
rate the veto exists to refuse.
"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache, _fw_band as B, _fw_port as P

EVERY, NET, GAIN = 4, 32, 0.10

class Probe(P.Tracker):
    def __init__(self):
        super().__init__(); self.adopts = []
    def sweep_start(self):
        self._narrow = (self.locked and not self.need_full and self.narrow_bpm > 0.0)
        super().sweep_start()
    def sweep_step(self):
        before = self.bpm
        super().sweep_step()
        if self.bpm != before:
            o = "narrow" if self._narrow else ("far" if self.sweep_took_far else "local")
            self.adopts.append((self.frame_index, self.bpm, o))

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

for margin in (0.20, 0.70):
    by = {"narrow":0, "local":0, "far":0}
    never = 0; refused_total = 0; harm = 0; harm_harmonic = 0
    for (x, y, ref) in pairs:
        t = make(2.0, margin); cut = len(x[1])
        t.run(np.concatenate((x[1], y[1])))
        log = np.array(t.bpm_log)
        old = log[cut-1]
        refused = [r for r in t.trace if r["frame"] >= cut and not r["took_far"]
                   and r["far_bpm"] > 0.0]
        refused_total += len(refused)
        got = None
        for r in refused:
            F = r["far_bpm"]
            for (f, bpm, o) in t.adopts:
                if f >= r["frame"] and abs(bpm - F) <= 0.5:
                    got = o; break
            if got: break
        if got: by[got] += 1
        else: never += 1
        # harm: the grid ended up AT a refused far rate
        for r in refused:
            if abs(log[-1] - r["far_bpm"]) <= 0.5:
                harm += 1
                if abs(r["far_bpm"] - old*4.0/3.0) <= 1.0: harm_harmonic += 1
                break
    print("MARGIN %.2f" % margin)
    print("  refusals: %d over %d pairs" % (refused_total, len(pairs)))
    print("  the grid arrived at a REFUSED far rate in %d of %d pairs: "
          "narrow %d, refused-local %d, approved-far %d, never %d"
          % (len(pairs)-never, len(pairs), by["narrow"], by["local"], by["far"], never))
    print("  the grid ENDED at a refused far rate in %d pairs, of which %d "
          "were the 4/3 harmonic of the old rate" % (harm, harm_harmonic))
    print()

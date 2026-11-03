#!/usr/bin/env python3
"""HARM ATTRIBUTION AT THE SHIPPED MARGIN.

Where the grid ends at a rate far from the truth, WHO took it there, and was
the evidence at the rate the grid ended on worse than the evidence at the
truth?  That is the harm the veto's margin exists to prevent.
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

for nb, tag in ((2.0, "narrow 2.0 (shipped)"), (0.25, "narrow 0.25 (walk frozen)")):
    print("=== %s" % tag)
    bad = 0; bypath = {"narrow":0,"local":0,"far":0,"none":0}
    for (x, y, ref) in pairs:
        t = make(nb, 0.70); cut = len(x[1])
        t.run(np.concatenate((x[1], y[1])))
        log = np.array(t.bpm_log)
        if abs(log[-1] - ref) > 1.0:
            bad += 1
            # who made the LAST move that took the grid away from the truth?
            # find the adopt that ended closest to the final rate
            last = None
            for (f, b, o) in t.adopts:
                if f >= cut and abs(b - log[-1]) <= 0.2:
                    last = o; break
            bypath[last or "none"] += 1
            # the narrow travel AFTER the last time the grid was within 1 BPM of the truth
            idx = np.where(np.abs(log[cut:] - ref) <= 1.0)[0]
            print("   %-40s ref %7.2f  grid ended %7.2f  last path %-6s  "
                  "the grid was AT the truth at %s"
                  % ("%s -> %s" % (x[0][:15], y[0][:15]), ref, log[-1], last,
                     ("%.1f s" % (idx[0]/P.RATE)) if len(idx) else "NEVER"))
    print("   ended away from the truth in %d of %d pairs; by path: %s"
          % (bad, len(pairs), bypath))
    print()

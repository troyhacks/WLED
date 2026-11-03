#!/usr/bin/env python3
"""DOES THE GRID PASS THROUGH A REFUSED RATE?

An adopt is a small hop, so 'arrived within 0.5 BPM on one adopt' misses a
walk that converges.  This measures the TRAJECTORY: at any frame after a
refusal, was the grid within 0.5 BPM of the refused candidate F?
And: at the shipped margin, is the new track's tempo itself one of the
refused far candidates (i.e. does the veto refuse the change the narrow
walk then makes anyway)?
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

for margin in (0.70,):
    print("MARGIN %.2f" % margin)
    passed = 0; ended = 0; refused_new = 0; new_refused = 0
    for (x, y, ref) in pairs:
        t = make(2.0, margin); cut = len(x[1])
        t.run(np.concatenate((x[1], y[1])))
        log = np.array(t.bpm_log)
        tail = log[cut:]
        refused = [r for r in t.trace if r["frame"] >= cut and not r["took_far"]
                   and r["far_bpm"] > 0.0]
        refused_total = len(refused)
        # was the NEW track's tempo among the refused far candidates?
        rn = [r for r in refused if abs(r["far_bpm"] - ref) <= 1.0]
        new_refused += len(rn)
        # did the grid PASS THROUGH any refused rate?
        hit = None
        for r in refused:
            F = r["far_bpm"]
            if np.any(np.abs(tail - F) <= 0.5):
                hit = ("passed", F, r["frame"]); break
        # did the grid END at a refused rate?
        eh = None
        for r in refused:
            if abs(log[-1] - r["far_bpm"]) <= 0.5:
                eh = r["far_bpm"]; break
        print("%-42s ref %7.2f  refusals %4d  new-tempo-refused %3d  "
              "grid %7.2f -> %7.2f  %s"
              % ("%s -> %s" % (x[0][:16], y[0][:16]), ref, refused_total,
                 len(rn), log[cut-1], log[-1],
                 ("PASSED THROUGH %7.2f" % hit[1]) if hit else
                 ("ended at %7.2f" % eh) if eh else "-"))
        if hit: passed += 1
        if eh: ended += 1
    print("  the grid PASSED THROUGH a refused rate in %d of %d pairs" % (passed, len(pairs)))
    print("  the grid ENDED at a refused rate in %d of %d pairs" % (ended, len(pairs)))
    print("  refusals whose candidate WAS the new track's tempo: %d" % new_refused)

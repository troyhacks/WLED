#!/usr/bin/env python3
"""THE WALK RATE, MEASURED IN THE WALK REGIME.

The reviewer's arithmetic: a narrow winner is at most 2.0 BPM from the grid,
the PLL takes 10% of the way, so 0.2 BPM per adopt; every 4 frames, at
93.75 fps, so 23.4 adopts/s, so 4.7 BPM/s, so 6 BPM in 1.3 s.

Two regimes are measured separately:
  (a) the window's best is NOT at the edge  (the truth is inside the window,
      or the score profile peaks in the middle)
  (b) the window's best IS at the edge      (the truth is outside)
and the step is broken down by whether the winner was at the edge.
"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache, _fw_band as B, _fw_port as P

EVERY, NET, GAIN = 4, 32, 0.10
NARROW = 2.0

class Probe(P.Tracker):
    def __init__(self):
        super().__init__(); self.adopts = []
    def sweep_start(self):
        self._narrow = (self.locked and not self.need_full and self.narrow_bpm > 0.0)
        self._win = None
        super().sweep_start()
    def sweep_step(self):
        before = self.bpm
        super().sweep_step()
        if self.bpm != before:
            o = "narrow" if self._narrow else ("far" if self.sweep_took_far else "local")
            # was the winning candidate at the window's edge?
            edge = None
            if self._narrow:
                idx = self.sweep_best_index
                lo = self.sweep_narrow_first; cnt = self.sweep_narrow_count
                edge = (idx == lo) or (idx == lo + cnt - 1)
            self.adopts.append((self.frame_index, self.bpm, o, edge))

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
    edge_steps = []; mid_steps = []
    edge_rate = []   # BPM/s over a 1 s window of edge-only adopts
    for (x, y, ref) in pairs:
        t = make(NARROW, margin); cut = len(x[1])
        t.run(np.concatenate((x[1], y[1])))
        prev = None; prevf = None
        for (f, b, o, e) in t.adopts:
            if f < cut: continue
            if o != "narrow": 
                prev = b; prevf = f; continue
            if prev is not None:
                d = b - prev
                (edge_steps if e else mid_steps).append(abs(d))
            prev = b; prevf = f
    def stat(v):
        if not v: return "n/a"
        v = np.array(v)
        return "n %5d  mean %.4f  max %.4f  p95 %.4f" % (len(v), v.mean(), v.max(), np.percentile(v,95))
    print("MARGIN %.2f" % margin)
    print("   narrow adopts with the winner AT THE WINDOW EDGE : %s" % stat(edge_steps))
    print("   narrow adopts with the winner in the middle      : %s" % stat(mid_steps))
    # what is the max sustained rate?  take the best 1 s of edge adopts in a pair
    best = 0.0; bestdesc = ""
    for (x, y, ref) in pairs:
        t = make(NARROW, margin); cut = len(x[1])
        t.run(np.concatenate((x[1], y[1])))
        ev = [(f, b) for (f, b, o, e) in t.adopts if f >= cut and o == "narrow" and e]
        for i in range(len(ev)):
            j = i
            while j < len(ev) and ev[j][0] - ev[i][0] <= P.RATE:
                j += 1
            if j - i < 4: continue
            d = ev[j-1][1] - ev[i][1]
            dt = (ev[j-1][0] - ev[i][0]) / P.RATE
            if dt >= 0.5 and abs(d/dt) > best:
                best = abs(d/dt); bestdesc = "%+.2f BPM in %.2f s over %d edge adopts" % (d, dt, j-i)
    print("   the fastest sustained edge-walk: %.2f BPM/s  (%s)" % (best, bestdesc))
    print()

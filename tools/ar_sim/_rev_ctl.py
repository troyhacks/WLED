#!/usr/bin/env python3
"""CONTROL: is the narrow WALK what causes the oscillation at margin 0.70?

Control 1: narrow window shrunk to 0.25 BPM (one candidate, the grid's own).
           The narrow search still runs every 4 frames, but it cannot walk.
Control 2: narrow window at 2.0 (the shipped one) - the walk is allowed.
If the oscillation follows the WINDOW and not the margin, the narrow path is
the cause.
"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache, _fw_band as B, _fw_port as P

EVERY, NET = 4, 32

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

def make(narrow_bpm, margin):
    t = Probe()
    t.narrow_bpm = narrow_bpm; t.search_every = EVERY; t.edge_rule = False
    t.collapse_trigger = True; t.full_every = NET
    t.net_margin = margin; t.veto_on_collapse = True
    t.exact_kernel = True
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

def stats(margin, nb):
    osc = 0; flips = []; reached = 0; err = []
    for (x, y, ref) in pairs:
        t = make(nb, margin); cut = len(x[1])
        t.run(np.concatenate((x[1], y[1])))
        log = np.array(t.bpm_log)
        side = np.sign(log[cut:] - ref); side = side[side != 0]
        f = sum(1 for i in range(1, len(side)) if side[i] != side[i-1])
        flips.append(f)
        if f >= 2: osc += 1
        # did the grid ever settle at the new rate (within 1 BPM) for 1 s?
        tail = log[cut:]
        if np.any(np.abs(tail - ref) <= 1.0): reached += 1
        err.append(abs(log[-1] - ref))
    return osc, flips, reached, float(np.mean(err))

for margin in (0.20, 0.70):
    for nb in (2.0, 0.25):
        osc, flips, reached, e = stats(margin, nb)
        print("margin %.2f  narrow window %.2f BPM   OSCILLATING %2d/14  reached-new %2d/14  "
              "mean |final-new| %.2f  flips %s" % (margin, nb, osc, reached, e, flips))

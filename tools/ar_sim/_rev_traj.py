#!/usr/bin/env python3
"""WHAT moves the grid across the gap in the narrow-only case: a walk, or a jump?

If it is a WALK, the grid's own log shows many small steps.  If it is a JUMP,
the log shows one large step, and the cause is the comb window rolling over:
the old track's peaks age out of the 12 s window, so the narrow window's best
becomes the new rate and the search adopts it in one go.
"""
import os, sys, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache, _fw_band as B, _fw_port as P

NARROW, EVERY, NET, MARGIN, GAIN = 2.0, 4, 32, 0.20, 0.10

class Probe(P.Tracker):
    def __init__(self):
        super().__init__()
        self.adopts = []
    def sweep_start(self):
        self._narrow = (self.locked and not self.need_full and self.narrow_bpm > 0.0)
        super().sweep_start()
    def sweep_step(self):
        before = self.bpm
        super().sweep_step()
        if self.bpm != before:
            self.adopts.append((self.frame_index, before, self.bpm, self._narrow))

def make(narrow_only):
    t = Probe()
    t.narrow_bpm = NARROW
    t.search_every = EVERY
    t.edge_rule = False
    t.collapse_trigger = not narrow_only
    t.full_every = 0 if narrow_only else NET
    t.net_margin = MARGIN
    t.veto_on_collapse = True
    t.pll_gain = GAIN
    t.exact_kernel = True
    return t

def reference_bpm(odf):
    t = P.Tracker(); t.narrow_bpm = 0.0; t.run(odf)
    return t.bpm if t.locked and t.bpm > 0 else None

def load_odfs(windows):
    cd = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_rev_odf_cache")
    os.makedirs(cd, exist_ok=True)
    items = []
    for k in _cache.keys()[:windows]:
        x = _cache.load(k)
        if x is None: continue
        cp = os.path.join(cd, k.replace("/", "_") + ".npy")
        if os.path.exists(cp): odf = np.load(cp)
        else:
            odf = B.firmware_odf(x, 80.0, 320.0); np.save(cp, odf)
        if len(odf) < 1200: continue
        items.append((k, odf))
    return items

items = load_odfs(30)
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

big = 0; small = 0; bigsteps = []
for (x, y, ref) in pairs:
    t = make(True)
    cut = len(x[1])
    t.run(np.concatenate((x[1], y[1])))
    ad = [(f, a, b) for (f, a, b, n) in t.adopts if f >= cut]
    if not ad: continue
    steps = [abs(b - a) for (_, a, b) in ad]
    mx = max(steps)
    bigsteps.append((mx, len(ad), ad))
    for s in steps:
        if s > 1.0: big += 1
        else: small += 1
    if mx > 1.0:
        f, a, b = [z for z in ad if abs(z[2]-z[1]) == mx][0]
        print("%-42s  %d adopts after the cut, biggest step %6.2f BPM "
              "(%.2f -> %.2f) at %.2f s" % (("%s -> %s" % (x[0][:17], y[0][:17])),
                                           len(ad), mx, a, b, (f-cut)/P.RATE))
print()
print("steps bigger than 1 BPM: %d     steps of 1 BPM or less: %d" % (big, small))
print("the biggest single step in any pair: %.2f BPM" % max(b[0] for b in bigsteps))

#!/usr/bin/env python3
"""THE VETO'S STATED PURPOSE.

From the firmware's own comment (ar_tempo.h, the veto block):

    "They disagree sometimes, and when they do the grid is dragged between
     them: the board was answering 125.00, then 131.00, then 125.00 again."

So the veto exists to stop the grid OSCILLATING between two rates.  The
reviewer's finding says the narrow path circumvents the veto's evidence rule
and walks the grid across the gap anyway.

If the reviewer's finding is a real defect, the grid must OSCILLATE: cross
the gap, and cross back.  If the grid CROSSES ONCE and SETTLES, the veto
did its job (it refused the jump) and the tracker converged - which is the
behaviour the firmware wants.

This measures, per pair, after the cut:
  - how many times the grid's sign of (grid - new rate) changes
  - whether the grid settles on one side
"""
import os, sys, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache, _fw_band as B, _fw_port as P

NARROW, EVERY, NET, GAIN = 2.0, 4, 32, 0.10

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
            self.adopts.append((self.frame_index, self.bpm,
                                "narrow" if self._narrow else
                                ("far" if self.sweep_took_far else "local")))

def make(margin, narrow_only=False):
    t = Probe()
    t.narrow_bpm = NARROW; t.search_every = EVERY; t.edge_rule = False
    t.collapse_trigger = not narrow_only
    t.full_every = 0 if narrow_only else NET
    t.net_margin = margin; t.veto_on_collapse = True
    t.pll_gain = GAIN; t.exact_kernel = True
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
        else: odf = B.firmware_odf(x, 80.0, 320.0); np.save(cp, odf)
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

for margin in (0.20, 0.70):
    for tag, no in (("FIRMWARE", False), ("NARROW ONLY", True)):
        osc = 0; settle = 0; cross_once = 0; nocross = 0; flips = []
        for (x, y, ref) in pairs:
            t = make(margin, narrow_only=no)
            cut = len(x[1])
            t.run(np.concatenate((x[1], y[1])))
            log = np.array(t.bpm_log)[cut:]
            # a coarse sign track: which side of the new rate is the grid on?
            side = np.sign(log - ref)
            side = side[side != 0]
            f = 0
            for i in range(1, len(side)):
                if side[i] != side[i-1]:
                    f += 1
            flips.append(f)
            if f >= 2: osc += 1
            elif f == 1: cross_once += 1
            else: nocross += 1
            if f == 1: settle += 1
        print("margin %.2f  %-12s  OSCILLATING (2+ crossings) %2d of %d   "
              "one crossing %2d   never %2d   flips: %s"
              % (margin, tag, osc, len(pairs), cross_once, nocross, flips))
    print()

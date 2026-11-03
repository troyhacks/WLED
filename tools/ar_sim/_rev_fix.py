#!/usr/bin/env python3
"""THE REVIEWER'S TWO PROPOSED FIXES, MEASURED, AT THE SHIPPED CONFIG.

FIX A: 'refuse to adopt an edge narrow winner unless its raw score beats
        the last approved rate's score by AR_TEMPO_NET_MARGIN'
FIX B: 'cap grid drift from the last approved rate'

Fix A is impossible as stated: inside a +/-2 BPM window the best
candidate's raw score is at most 1.33x the grid's own rate's score
(median 1.0000), so a 1.70x rule is never satisfiable.

Fix B is measured here: a cap on how far the grid may drift from the
last rate a whole-band search APPROVED.
"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache, _fw_band as B, _fw_port as P
HERE = os.path.dirname(os.path.abspath(__file__))
cd = os.path.join(HERE, "_rev_odf_cache")

class Probe(P.Tracker):
    def __init__(self):
        super().__init__(); self._adopts = []
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

class Capped(Probe):
    """FIX B: the grid may not drift more than CAP BPM from the last rate a
    whole-band search APPROVED."""
    cap = 3.0
    def __init__(self):
        super().__init__(); self._anchor = 0.0; self.blocked = 0
    def sweep_start(self):
        self._narrow = (self.locked and not self.need_full and self.narrow_bpm > 0.0)
        super().sweep_start()
    def sweep_step(self):
        before = self.bpm
        super().sweep_step()
        if self.bpm != before and self._narrow:
            if self._anchor > 0.0 and abs(self.bpm - self._anchor) > self.cap:
                self.bpm = self._anchor
                self.period = 60.0 * P.RATE / self.bpm
                self.blocked += 1
        # an APPROVED far rate re-anchors
        if (not self._narrow) and self.sweep_took_far:
            self._anchor = self.bpm

def run(cls, cap=None):
    reached = 0; err = []; blocked = 0; final_bad = 0
    for (a, b, r) in pairs:
        t = cls()
        if cap is not None:
            t.cap = cap; t._anchor = 0.0; t.blocked = 0
        t.narrow_bpm = 2.0; t.search_every = 4; t.edge_rule = False
        t.collapse_trigger = True; t.full_every = 32
        t.net_margin = 0.70; t.veto_on_collapse = True; t.exact_kernel = True
        cut = len(a[1])
        t.run(np.concatenate((a[1], b[1])))
        log = np.array(t.bpm_log)[cut:]
        if np.any(np.abs(log - r) <= 1.0): reached += 1
        err.append(abs(log[-1] - r))
        blocked += getattr(t, "blocked", 0)
        if abs(log[-1] - r) > 5.0: final_bad += 1
    return reached, float(np.mean(err)), blocked, final_bad

r, e, b, fb = run(Probe)
print("SHIPPED (no cap)                      reached-new %2d/14  mean |final-new| %6.2f  "
      "ended >5 BPM off %2d/14  blocked 0" % (r, e, fb))
for cap in (2.0, 3.0, 5.0):
    r, e, b, fb = run(Capped, cap)
    print("FIX B (cap %.1f BPM from the last APPROVED rate)  reached-new %2d/14  "
          "mean |final-new| %6.2f  ended >5 BPM off %2d/14  blocked %d"
          % (cap, r, e, fb, b))

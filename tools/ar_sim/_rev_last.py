#!/usr/bin/env python3
"""TWO QUESTIONS, ONE RUN.

1. WHEN THE NARROW WALK IS FROZEN (window 0.25 BPM), what path carries
   the grid to the new track's rate?  If it is the veto-approved far path,
   the narrow walk is NOT load-bearing and the reviewer's premise ("the
   narrow search walks it, unchecked") is about a path that does not have
   to do the walking.

2. WHAT DO THE 11 SIGN-CROSSINGS LOOK LIKE?  The sign-change counter
   says 11-12/14 pairs cross the reference repeatedly; the hysteresis
   counter says 0/14 stray more than 0.5 BPM past it on both sides.  The
   trajectory settles it.
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

print("=" * 100)
print("1. WITH THE NARROW WALK FROZEN (window 0.25 BPM): WHAT CARRIES THE GRID?")
print("=" * 100)
by_path = {"narrow": 0, "far": 0, "local": 0, "none": 0}
for (a, b, r) in pairs:
    t = Probe(); t._adopts = []
    t.narrow_bpm = 0.25; t.search_every = 4; t.edge_rule = False
    t.collapse_trigger = True; t.full_every = 32
    t.net_margin = 0.70; t.veto_on_collapse = True; t.exact_kernel = True
    cut = len(a[1])
    t.run(np.concatenate((a[1], b[1])))
    hit = None
    for (f, old, new, nw) in t._adopts:
        if f >= cut and abs(new - r) <= 1.0:
            hit = ("narrow" if nw else ("far" if t.sweep_took_far else "local"), f)
            break
    k = hit[0] if hit else "none"
    by_path[k] += 1
    print("  %-40s truth %7.2f  arrived by %s%s"
          % ("%s -> %s" % (a[0][:16], b[0][:16]), r, k,
             (" at %.2f s" % ((hit[1] - cut) / P.RATE)) if hit else ""))
print("  by path: %s" % by_path)

print()
print("=" * 100)
print("2. THE TRAJECTORY OF A PAIR WITH MANY SIGN-CROSSINGS")
print("=" * 100)
best = None
for (a, b, r) in pairs:
    t = P.Tracker()
    t.narrow_bpm = 2.0; t.search_every = 4; t.edge_rule = False
    t.collapse_trigger = True; t.full_every = 32
    t.net_margin = 0.70; t.veto_on_collapse = True; t.exact_kernel = True
    cut = len(a[1])
    t.run(np.concatenate((a[1], b[1])))
    log = np.array(t.bpm_log)[cut:]
    side = np.sign(log - r); side = side[side != 0]
    cross = sum(1 for i in range(1, len(side)) if side[i] != side[i-1])
    if best is None or cross > best[0]:
        best = (cross, log, r, a[0], b[0], cut)
cross, log, r, an, bn, cut = best
print("%s -> %s   truth %.2f   %d sign-crossings" % (an[:24], bn[:24], r, cross))
for s in range(0, int(len(log)/P.RATE), 2):
    v = log[min(int(s*P.RATE), len(log)-1)]
    print("   %5.1f s  %8.3f  %+7.3f" % (s, v, v - r))

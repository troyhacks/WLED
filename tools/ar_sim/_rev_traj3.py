#!/usr/bin/env python3
"""RESOLVE: 11 SIGN-CROSSINGS vs 1 HYSTERESIS FLIP.

The ctl counter says the grid crosses the reference 11 times in some
pairs; the hysteresis counter says it strays more than 0.5 BPM past the
reference on both sides only once.  That means the grid JITTERS across the
answer rather than fighting between two rates.

This dumps the actual trajectory, and the strays, for the pairs with the
most crossings - because the veto exists to stop the "125 then 131 then
125" fight, and that is the difference between the two.
"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache, _fw_band as B, _fw_port as P
HERE = os.path.dirname(os.path.abspath(__file__))
cd = os.path.join(HERE, "_rev_odf_cache")

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

rows = []
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
    stray = float(np.max(np.abs(log - r)))
    rows.append((cross, stray, log, a[0], b[0], r, cut))

rows.sort(key=lambda x: -x[0])
print("%-40s %6s %8s %8s" % ("pair", "cross", "max stray", "settled"))
for (cross, stray, log, an, bn, r, cut) in rows:
    print("%-40s %6d %8.2f %8.2f" % ("%s -> %s" % (an[:16], bn[:16]), cross, stray, log[-1]))
print()
cross, stray, log, an, bn, r, cut = rows[0]
print("THE WORST PAIR: %s -> %s   new track's rate %.2f" % (an[:24], bn[:24], r))
print("  the grid, every second, after the cut:")
for s in range(0, int(len(log)/P.RATE), 2):
    v = log[min(int(s*P.RATE), len(log)-1)]
    bar = "#" * int(min(abs(v - r), 20))
    print("   %5.1f s  %8.3f  %+7.3f  %s" % (s, v, v - r, bar))

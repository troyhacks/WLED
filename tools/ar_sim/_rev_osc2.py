#!/usr/bin/env python3
"""CHARACTERISE THE OSCILLATION: whose path, and what it looks like."""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache, _fw_band as B, _fw_port as P

NARROW, EVERY, NET, GAIN = 2.0, 4, 32, 0.10
MARGIN = float(os.environ.get("MARGIN", "0.70"))

class Probe(P.Tracker):
    def __init__(self):
        super().__init__(); self.adopts = []
    def sweep_start(self):
        self._narrow = (self.locked and not self.need_full and self.narrow_bpm > 0.0)
        super().sweep_start()
    def sweep_step(self):
        before = self.bpm; o = None
        super().sweep_step()
        if self.bpm != before:
            o = "narrow" if self._narrow else ("far" if self.sweep_took_far else "local")
            self.adopts.append((self.frame_index, self.bpm, o))

def make(margin):
    t = Probe()
    t.narrow_bpm = NARROW; t.search_every = EVERY; t.edge_rule = False
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

# which pairs oscillate, and the net travel by path
print("MARGIN %.2f" % MARGIN)
for idx, (x, y, ref) in enumerate(pairs):
    t = make(MARGIN); cut = len(x[1])
    t.run(np.concatenate((x[1], y[1])))
    log = np.array(t.bpm_log)
    side = np.sign(log[cut:] - ref); side = side[side != 0]
    flips = sum(1 for i in range(1, len(side)) if side[i] != side[i-1])
    # net travel by path
    nt = 0.0; lt = 0.0; ft = 0.0
    prev = log[cut-1]
    steps = []
    for (f, b, o) in t.adopts:
        if f < cut: continue
        d = b - prev
        if o == "narrow": nt += d
        elif o == "local": lt += d
        else: ft += d
        steps.append((f-cut, b, o, d))
        prev = b
    mx = max((abs(s[3]) for s in steps), default=0.0)
    nst = len(steps)
    print("%2d %-40s ref %7.2f  flips %2d  travel narrow %+7.2f  refused-local %+7.2f  far %+7.2f  | max step %.3f  adopts %d"
          % (idx, ("%s -> %s" % (x[0][:15], y[0][:15])), ref, flips, nt, lt, ft, mx, nst))
    if idx == 1 or flips >= 6:
        # print the trajectory sampled
        print("      trace (s, bpm, origin, step):")
        shown = 0
        for (f, b, o, d) in steps:
            if shown >= 26: print("        ..."); break
            print("        %7.2f s  %8.3f  %-6s %+7.4f" % (f/P.RATE, b, o, d))
            shown += 1

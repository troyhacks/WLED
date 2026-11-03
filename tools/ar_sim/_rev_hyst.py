#!/usr/bin/env python3
"""HYSTERESIS OSCILLATION: a real flip needs the grid to go 0.5 BPM PAST the
reference on one side, then 0.5 BPM PAST it on the other side.  Jitter that
sits on the answer does not count."""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache, _fw_band as B, _fw_port as P

EVERY, NET = 4, 32
HYST = 0.5

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

def hyst_flips(log, ref, h=HYST):
    """count side changes with hysteresis h"""
    side = 0; flips = 0
    for v in log:
        if v > ref + h:
            s = 1
        elif v < ref - h:
            s = -1
        else:
            continue
        if s != 0 and side != 0 and s != side:
            flips += 1
        side = s
    return flips

def run(margin, nb):
    osc = 0; flips = []; reached = 0; err = []
    for (x, y, ref) in pairs:
        t = make(nb, margin); cut = len(x[1])
        t.run(np.concatenate((x[1], y[1])))
        log = np.array(t.bpm_log)[cut:]
        f = hyst_flips(log, ref)
        flips.append(f)
        if f >= 2: osc += 1
        if np.any(np.abs(log - ref) <= 1.0): reached += 1
        err.append(abs(log[-1] - ref))
    return osc, flips, reached, float(np.mean(err))

for margin in (0.20, 0.70):
    for nb in (2.0, 0.25):
        osc, flips, reached, e = run(margin, nb)
        print("margin %.2f  narrow %.2f  HYST-flips>=2: %2d/14  reached-new %2d/14  "
              "mean |final-new| %6.2f  flips %s" % (margin, nb, osc, reached, e, flips))
print()

# raw trajectory of the worst pair at margin 0.70
x, y, ref = pairs[8]
t = make(2.0, 0.70); cut = len(x[1])
t.run(np.concatenate((x[1], y[1])))
log = np.array(t.bpm_log)[cut:]
print("WORST PAIR %s -> %s   reference for the new track %.2f   old grid %.2f"
      % (x[0][:20], y[0][:20], ref, log[0]))
for s in range(0, 40, 2):
    print("  %5.1f s   %8.3f" % (s, log[min(int(s*P.RATE), len(log)-1)]))

#!/usr/bin/env python3
"""The veto at the CURRENT margin (0.70): does it hold, and does the narrow
path bypass it?

Three questions, one script:
  1. THE FIRMWARE CONFIG: does the grid reach the new rate, and if it does,
     by which path (far = the veto APPROVED, narrow = unchecked, local = a
     REFUSED whole-band search's local best)?
  2. Does the grid ever arrive at a rate the veto REFUSED, and by which path?
  3. NARROW ONLY (no whole-band search at all): does the grid cross the gap?
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
    print("#" * 96)
    print("MARGIN %.2f   (ratio bar %.2f)" % (margin, 1.0 + margin))
    print("#" * 96)
    reached = 0; by = {"narrow":0, "far":0, "local":0, "none":0}
    bypass = {"narrow":0, "local":0, "far":0, "none":0}
    for (x, y, ref) in pairs:
        t = make(margin)
        cut = len(x[1])
        t.run(np.concatenate((x[1], y[1])))
        log = np.array(t.bpm_log)
        hit = None
        for (f, b, o) in t.adopts:
            if f >= cut and abs(b - ref) <= 1.0:
                hit = (f-cut)/P.RATE, o; break
        if hit: reached += 1; by[hit[1]] += 1
        else: by["none"] += 1
        # did the grid arrive at a rate the veto REFUSED?
        refused = [r for r in t.trace if r["frame"] >= cut and not r["took_far"]
                   and r["far_bpm"] > 0.0]
        bp = "none"
        for r in refused:
            for (f, b, o) in t.adopts:
                if f >= r["frame"] and abs(b - r["far_bpm"]) <= 0.5:
                    bp = o; break
            if bp != "none": break
        bypass[bp] += 1
    print("  Q1 the grid reached the NEW rate in %d of %d pairs  (narrow %d, "
          "veto-approved far %d, refused-local %d, never %d)"
          % (reached, len(pairs), by["narrow"], by["far"], by["local"], by["none"]))
    print("  Q2 the grid arrived at a REFUSED far rate in %d of %d pairs  "
          "(by narrow %d, by a refused local %d, by an approved far %d, never %d)"
          % (len(pairs)-bypass["none"], len(pairs), bypass["narrow"],
             bypass["local"], bypass["far"], bypass["none"]))
    # Q3
    cross = 0; steps = []
    for (x, y, ref) in pairs:
        t = make(margin, narrow_only=True)
        cut = len(x[1])
        t.run(np.concatenate((x[1], y[1])))
        log = np.array(t.bpm_log)
        if any(abs(log[f]-ref) <= 1.0 for f in range(cut, len(log))): cross += 1
        for (f, b, o) in t.adopts:
            if f >= cut: steps.append(abs(b))
    print("  Q3 NARROW ONLY: the grid crossed the gap in %d of %d pairs"
          % (cross, len(pairs)))
    print(flush=True)
print("%.0f s" % (time.time() - time.time()))

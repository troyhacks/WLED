#!/usr/bin/env python3
"""THE DECISIVE MEASUREMENT.

The file's own stated invariant (ar_tempo.h 129-131):
    'A change is taken when the new tempo's evidence is far enough ahead to
     clear the margin, and not otherwise.'
and (ar_tempo.h 312):
    'it is the only search that can move the grid a long way.'

So measure, over the corpus, at the SHIPPED margin:
  A. how many whole-band searches APPROVED the far rate (the veto fired),
     and how many REFUSED it;
  B. how many pairs the grid's rate changed by more than 1 BPM, and WHICH
     path moved it;
  C. how many of those changes were to a rate the veto had REFUSED as a far
     candidate - the circumvention the reviewer names;
  D. the reviewer's cited pair, 119.89 -> 130.00, in detail.
"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache, _fw_band as B, _fw_port as P

EVERY, NET = 4, 32   # the GAIN and the MARGIN now come from the port's defaults

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

for margin in (0.20, 0.70):
    appr = 0; rej = 0
    changed = 0; by = {"narrow":0,"local":0,"far":0,"none":0}
    circ = 0; circ_detail = []
    for (x, y, ref) in pairs:
        t = make(2.0, margin); cut = len(x[1])
        t.run(np.concatenate((x[1], y[1])))
        log = np.array(t.bpm_log)
        for r in t.trace:
            if r["frame"] < cut: continue
            if r["took_far"]: appr += 1
            else: rej += 1
        # a "change": the grid moved by more than 1 BPM from the cut value
        moved = np.where(np.abs(log[cut:] - log[cut-1]) > 1.0)[0]
        if len(moved):
            changed += 1
            f = cut + moved[0]
            path = "none"
            for (ff, b, o) in t.adopts:
                if ff >= f: path = o; break
            by[path] += 1
        # circumvention: the grid passed through a REFUSED far candidate
        refused = [r for r in t.trace if r["frame"] >= cut and not r["took_far"]
                   and r["far_bpm"] > 0.0]
        tail = log[cut:]
        hit = None
        for r in refused:
            F = r["far_bpm"]
            w = np.where(np.abs(tail - F) <= 0.5)[0]
            if len(w):
                # which path moved it there?
                pth = "none"
                for (ff, b, o) in t.adopts:
                    if ff >= cut + w[0] - 20 and abs(b - F) <= 0.5:
                        pth = o; break
                hit = (F, pth); break
        if hit:
            circ += 1
            circ_detail.append((x[0][:14], y[0][:14], hit[0], hit[1]))
    print("MARGIN %.2f" % margin)
    print("  A. whole-band searches after the cut: APPROVED %d, REFUSED %d  "
          "(the veto fired %d times, i.e. %d%% of the time)"
          % (appr, rej, appr, 100.0*appr/max(1,appr+rej)))
    print("  B. the grid moved more than 1 BPM in %d of %d pairs; by path: %s"
          % (changed, len(pairs), by))
    print("  C. the grid PASSED THROUGH a rate the veto REFUSED, in %d of %d "
          "pairs: %s" % (circ, len(pairs), circ_detail[:6]))
    print()

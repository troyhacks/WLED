#!/usr/bin/env python3
"""Does the NARROW path alone carry the grid across a track-change gap?

The reviewer's corpus claim: with the whole-band search removed entirely
(full_every = 0, collapse off), 8 of 14 track changes still carried the grid
across the gap.  Measured here on the same corpus, both ways.
"""
import argparse, os, sys, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache, _fw_band as B, _fw_port as P

NARROW, EVERY, NET, MARGIN, GAIN = 2.0, 4, 32, 0.20, 0.10

def make(narrow_only):
    t = P.Tracker()
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

ap = argparse.ArgumentParser()
ap.add_argument("--windows", type=int, default=30)
ap.add_argument("--pairs", type=int, default=14)
ap.add_argument("--gap", type=float, default=8.0)
a = ap.parse_args()

t0 = time.time()
items = load_odfs(a.windows)
refs = {k: reference_bpm(o) for (k, o) in items}
pairs = []
for i in range(len(items)):
    ra = refs.get(items[i][0])
    if ra is None: continue
    for j in range(len(items)):
        if j == i: continue
        rb = refs.get(items[j][0])
        if rb is None or abs(rb - ra) < a.gap: continue
        pairs.append((items[i], items[j], rb)); break
pairs = pairs[:a.pairs]
print("%d pairs  %.0f s" % (len(pairs), time.time()-t0)); print()

for tag, narrow_only in (("THE FIRMWARE CONFIGURATION", False),
                         ("NARROW ONLY (no whole-band search at all)", True)):
    reached = 0; by_narrow = 0; rows = []
    for (x, y, ref) in pairs:
        t = make(narrow_only)
        cut = len(x[1])
        t.run(np.concatenate((x[1], y[1])))
        log = np.array(t.bpm_log)
        hit = None
        for f in range(cut, len(log)):
            if abs(log[f] - ref) <= 1.0:
                hit = (f - cut) / P.RATE; break
        if hit is not None: reached += 1
        rows.append((("%s -> %s" % (x[0][:17], y[0][:17])), log[cut-1], ref,
                     hit, log[-1]))
    print("=" * 100)
    print(tag)
    print("=" * 100)
    for (nm, g0, ref, hit, last) in rows:
        print("  %-42s grid %7.2f  new %7.2f  reached: %s   final %7.2f"
              % (nm, g0, ref, ("%.1f s" % hit) if hit is not None else "NO", last))
    print("  THE GRID CROSSED THE GAP IN %d of %d PAIRS" % (reached, len(pairs)))
    print(flush=True)
print("%.0f s" % (time.time()-t0))

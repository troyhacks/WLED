#!/usr/bin/env python3
"""THE REVIEWER'S MECHANISM AT THE CURRENT MARGIN.

Two questions:
  A. Does the grid reach a rate the veto REFUSED, via the narrow path?
     (the reviewer's circumvention)
  B. Does the grid OSCILLATE - cross the gap and come back - which is the
     symptom the veto exists to prevent?  And if it oscillates, whose path
     does it?  The narrow search's, or the veto's OWN refusal path (which
     adopts the best rate inside the window)?
"""
import os, sys, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache, _fw_band as B, _fw_port as P

NARROW, EVERY, NET, GAIN = 2.0, 4, 32, 0.10
MARGIN = float(os.environ.get("MARGIN", "0.70"))

class Probe(P.Tracker):
    def __init__(self, freeze_narrow=False, freeze_refusal=False):
        super().__init__()
        self.freeze_narrow = freeze_narrow
        self.freeze_refusal = freeze_refusal
        self.adopts = []
    def sweep_start(self):
        self._narrow = (self.locked and not self.need_full and self.narrow_bpm > 0.0)
        super().sweep_start()
    def sweep_step(self):
        pre = (self.period, self.bpm, self.next_beat)
        before = self.bpm
        super().sweep_step()
        if self.freeze_narrow and self._narrow:
            self.period, self.bpm, self.next_beat = pre
        elif self.freeze_refusal and not self._narrow and not self.sweep_took_far:
            self.period, self.bpm, self.next_beat = pre
        if self.bpm != before:
            self.adopts.append((self.frame_index, self.bpm,
                                "narrow" if self._narrow else
                                ("far" if self.sweep_took_far else "local")))

def make(margin, narrow_only=False, fn=False, fr=False):
    t = Probe(fn, fr)
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

def run(t, x, y):
    cut = len(x[1])
    t.run(np.concatenate((x[1], y[1])))
    return cut, np.array(t.bpm_log)

print("MARGIN %.2f" % MARGIN)
print()
# A. does the grid reach a REFUSED far rate, by which path?
rej = {"narrow":0, "local":0, "far":0, "none":0}
refused_new = 0; got_new_by_narrow = 0
for (x, y, ref) in pairs:
    t = make(MARGIN)
    cut, log = run(t, x, y)
    refused = [r for r in t.trace if r["frame"] >= cut and not r["took_far"]
               and r["far_bpm"] > 0.0]
    refused_new += len([r for r in refused if abs(r["far_bpm"]-ref) <= 1.0])
    bp = "none"
    for r in refused:
        for (f, b, o) in t.adopts:
            if f >= r["frame"] and abs(b - r["far_bpm"]) <= 0.5:
                bp = o; break
        if bp != "none": break
    rej[bp] += 1
    # did the grid get to the NEW rate despite refusals, by narrow?
    rn = [r for r in refused if abs(r["far_bpm"]-ref) <= 1.0]
    if rn:
        for (f, b, o) in t.adopts:
            if f >= rn[0]["frame"] and abs(b-ref) <= 1.0:
                if o == "narrow": got_new_by_narrow += 1
                break
print("A. the veto refused the NEW rate %d times over %d pairs" % (refused_new, len(pairs)))
print("   of those pairs, the grid reached the new rate BY NARROW: %d" % got_new_by_narrow)
print("   the grid arrived at a REFUSED far rate: %d of %d pairs (narrow %d, "
      "refused-local %d, approved-far %d, never %d)"
      % (len(pairs)-rej["none"], len(pairs), rej["narrow"], rej["local"],
         rej["far"], rej["none"]))
print()

# B. oscillation, and whose path
for tag, fn, fr in (("FIRMWARE", False, False),
                    ("narrow path FROZEN", True, False),
                    ("narrow + refusal path FROZEN", True, True)):
    osc = 0; flips = []
    for (x, y, ref) in pairs:
        t = make(MARGIN, fn=fn, fr=fr)
        cut, log = run(t, x, y)
        side = np.sign(log[cut:] - ref)
        side = side[side != 0]
        f = sum(1 for i in range(1, len(side)) if side[i] != side[i-1])
        flips.append(f)
        if f >= 2: osc += 1
    print("B. %-32s OSCILLATING %2d of %d   flips: %s"
          % (tag, osc, len(pairs), flips))

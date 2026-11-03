#!/usr/bin/env python3
"""THE CONCRETE HARM OF THE UNCHECKED NARROW WALK.

The walk can carry the grid to a rate that is NOT the music's rate - a
harmonic of it.  The veto's evidence rule exists to stop exactly that
(the header says the far rate the safety net nominates is "the FOUR-THIRDS
harmonic of the rate the grid is on").  But the veto is only asked about
WHOLE-BAND searches.

For every pair this reports the grid's final rate, the truth, the
harmonic ratio, and whether the veto had REFUSED that rate.
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

bad = 0
for (a, b, r) in pairs:
    t = Probe(); t._adopts = []
    t.narrow_bpm = 2.0; t.search_every = 4; t.edge_rule = False
    t.collapse_trigger = True; t.full_every = 32
    t.net_margin = 0.70; t.veto_on_collapse = True; t.exact_kernel = True
    cut = len(a[1])
    t.run(np.concatenate((a[1], b[1])))
    log = np.array(t.bpm_log)[cut:]
    end = log[-1]
    err = abs(end - r)
    # the harmonic ratios
    rat = end / r if r else 0
    harm = ""
    for num, den in ((7,6),(4,3),(3,2),(5,4),(9,8),(8,5)):
        if abs(rat - num/den) < 0.06:
            harm = "%d/%d" % (num, den)
    refused = [tr["far_bpm"] for tr in t.trace
               if tr["frame"] >= cut and not tr["took_far"] and tr["far_bpm"] > 0]
    was_refused = any(abs(x - end) <= 0.5 for x in refused)
    # which path carried it to the end
    path = "?"
    for (f, old, new, nw) in t._adopts:
        if abs(new - end) <= 0.01:
            path = "narrow" if nw else ("far" if t.sweep_took_far else "local")
            break
    tag = "  <-- WRONG RATE" if err > 5.0 else ""
    if err > 5.0: bad += 1
    print("%-38s truth %7.2f  end %7.2f  err %6.2f  ratio %5.3f %-4s  "
          "veto had REFUSED this rate: %-3s  carried by %s%s"
          % ("%s -> %s" % (a[0][:14], b[0][:14]), r, end, err, rat, harm,
             "YES" if was_refused else "no", path, tag))
print()
print("the grid ENDED more than 5 BPM from the truth in %d of %d pairs" % (bad, len(pairs)))

import os, sys, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache, _fw_band as B, _fw_port as P
from _diag_phase import kick_onsets

def shipped(margin=0.70, gain=0.20, prior=0.6, narrow_prior=False, lo=90.0, hi=320.0):
    t = P.Tracker()
    t.narrow_bpm = 2.0; t.search_every = 4; t.edge_rule = False
    t.collapse_trigger = True; t.full_every = 32
    t.net_margin = margin; t.pll_gain = gain; t.prior_weight = prior
    t.prior = t._prior_table(); t.prior_in_narrow = narrow_prior
    return t

rows = []
for k in _cache.keys()[:24]:
    x = _cache.load(k)
    if x is None: continue
    odf = B.firmware_odf(x, 90.0, 320.0)
    if len(odf) < 1200: continue
    # the PORT'S DEFAULTS (what a script gets from a bare Tracker())
    a = P.Tracker(); a.run(odf)
    # the SHIPPED firmware config
    b = shipped(); b.run(odf)
    rows.append((a.bpm, b.bpm, a.net_far, b.net_far, len(a.beats), len(b.beats)))
print("windows:", len(rows))
agree = sum(1 for r in rows if abs(r[0]-r[1]) < 0.5)
print("published rate agrees within 0.5 BPM: %d/%d" % (agree, len(rows)))
d = [abs(r[0]-r[1]) for r in rows if r[0]>0 and r[1]>0]
print("median |diff| = %.3f   max = %.3f" % (float(np.median(d)), max(d)))
print("far moves  port-default: %.1f   shipped: %.1f" % (np.mean([r[2] for r in rows]), np.mean([r[3] for r in rows])))
print("beats  port-default med %.0f   shipped med %.0f" % (np.median([r[4] for r in rows]), np.median([r[5] for r in rows])))

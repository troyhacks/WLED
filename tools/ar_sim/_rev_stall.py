import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _fw_port as P
from _rev_walk_synth import beat_odf, place_locked, cfg

truth = 131.0
odf = beat_odf(truth, 30.0, noise=0.02, seed=1)

t = cfg(full_every=0, collapse=False)
place_locked(t, 125.0)
t.run(odf)
nar = [a for a in t.adopts if a[4]]
print("FIRST 14 NARROW ADOPTS (grid placed at 125.00, truth 131.00)")
for (f, b, lo, hi, _) in nar[:14]:
    print("  frame %5d (%.2f s)  grid %8.3f  window [%.2f,%.2f]  step %+.4f"
          % (f, f/P.RATE, b, lo, hi, b - 125.0 if f == nar[0][0] else b - nar[nar.index((f,b,lo,hi,_))-1][1]))
print()
print("LAST 8 NARROW ADOPTS")
for (f, b, lo, hi, _) in nar[-8:]:
    print("  frame %5d (%.2f s)  grid %8.3f  window [%.2f,%.2f]"
          % (f, f/P.RATE, b, lo, hi))
print()

# what does the narrow search nominate once the grid has stalled?
frames = np.array([j for j,_ in t.peak_frame], dtype=float)
strs  = np.array([s for _,s in t.peak_frame], dtype=float)
bps = np.arange(90.0, 175.01, 0.25)
sc, ph = t.comb_scores_all(frames, strs, bps)
for grid in (125.0, 127.0, 129.75, 130.5):
    f, c = P.narrow_window(grid, 2.0)
    idx = np.arange(f, f+c)
    win = bps[idx]
    j = int(idx[np.argmax(sc[idx])])
    jv = int(idx[np.argmax(sc[idx] - t.prior[idx])])
    print("grid %7.2f  window [%7.2f,%7.2f]  best RAW %7.2f (score %.4f)   best VALUE %7.2f (score %.4f)"
          % (grid, win[0], win[-1], bps[j], sc[j], bps[jv], sc[jv]))
print()
print("the score profile near the truth:")
for b in (125.0, 127.0, 128.0, 129.0, 129.75, 130.25, 130.5, 130.75, 131.0, 131.25):
    k = int(round((b-90.0)/0.25))
    print("   %7.2f  score %.4f" % (bps[k], sc[k]))

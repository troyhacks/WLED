import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _fw_port as P
from _rev_walk_synth import beat_odf, place_locked, cfg

truth = 131.0
odf = beat_odf(truth, 60.0, noise=0.02, seed=1)

for tag, fn in (("narrow only (no whole-band search)", cfg(full_every=0, collapse=False)),
                ("the firmware configuration", cfg())):
    t = fn
    place_locked(t, 125.0)
    t.run(odf)
    log = np.array(t.bpm_log)
    print(tag)
    for m in (1.3, 2.0, 5.0, 10.0, 20.0, 30.0, 45.0, 59.0):
        f = int(m * P.RATE)
        if f < len(log):
            print("   at %5.1f s the grid is %8.3f  (%+.2f from the truth)" % (m, log[f], log[f]-truth))
    first = None
    for f in range(len(log)):
        if abs(log[f]-truth) <= 0.5:
            first = f
            break
    print("   first within 0.5 BPM of the truth: %s" % ("%.2f s" % (first/P.RATE) if first is not None else "NEVER"))
    print("   final %.3f (%+.3f)   whole-band searches %d  vetoes %d  far wins %d"
          % (log[-1], log[-1]-truth, t.search_full, t.net_vetoes, t.net_far))
    print()

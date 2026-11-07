#!/usr/bin/env python3
"""The comb's score profile across the band, for a given ODF."""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _fw_bg as B                                   # noqa: E402
import _fw_port as P                                 # noqa: E402

odf = B.beat_odf([(128.0, 0.0, 15.0, 1.0),
                  (127.0, 15.0, 3.0, 1.0),
                  (128.0, 18.0, 15.0, 1.0)], dur=33.0)

t = B.BgTracker()
# the onsets the tracker would have at 18 s (the twitch is in the window)
for i in range(int(18 * P.RATE)):
    t.frame(float(odf[i]))
print("locked", t.locked, "bpm", t.bpm)
cut = t.frame_index - t.bg_window
keep = [(f, s) for (f, s) in t.peak_frame if f >= cut]
fr = np.array([f for f, _ in keep], float)
st = np.array([s for _, s in keep], float)
print("onsets in the window:", len(fr))

bpms = np.arange(120.0, 136.01, 0.5)
sc, ph = t.comb_scores_all(fr, st, bpms)
for b, s in zip(bpms, sc):
    bar = "#" * int(round(s * 40))
    print("  %6.1f  %.4f  %s" % (b, s, bar))

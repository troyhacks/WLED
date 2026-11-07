#!/usr/bin/env python3
"""The mixture's profile: what does the comb see at t=22 of the twitch?"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _fw_bg as B                                   # noqa: E402
import _fw_port as P                                 # noqa: E402

mix = B.beat_odf([(128.0, 0.0, 15.0, 1.0),
                  (127.0, 15.0, 3.0, 1.0),
                  (128.0, 18.0, 15.0, 1.0)], dur=33.0)

t = B.BgTracker()
for i in range(int(22 * P.RATE)):
    t.frame(float(mix[i]))
cut = t.frame_index - t.bg_window
keep = [(f, s) for (f, s) in t.peak_frame if f >= cut]
fr = np.array([f for f, _ in keep], float)
st = np.array([s for _, s in keep], float)
print("onsets (frame, s):", [(round(f / P.RATE, 3), round(s, 3)) for f, s in keep])
bpms = np.arange(122.0, 134.01, 0.25)
sc, ph = t.comb_scores_all(fr, st, bpms)
for b, s in zip(bpms, sc):
    print("  %6.2f  %.4f  %s" % (b, s, "#" * int(round(s * 40))))

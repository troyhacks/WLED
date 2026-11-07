#!/usr/bin/env python3
"""The whole-band profile for a case, at a given second."""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _fw_bg as B                                   # noqa: E402
import _fw_port as P                                 # noqa: E402

case = sys.argv[1]
at = float(sys.argv[2])

if case == "breakout":
    odf = B.beat_odf([(128.0, 0.0, 30.0, 1.0),
                      (160.0, 0.0, 30.0, 1.0)], dur=30.0)
elif case == "pure":
    odf = B.beat_odf([(128.0, 0.0, 30.0, 1.0)], dur=30.0)
else:
    raise SystemExit("no such case")

t = B.BgTracker()
for i in range(int(at * P.RATE)):
    t.frame(float(odf[i]))
cut = t.frame_index - t.bg_window
keep = [(f, s) for (f, s) in t.peak_frame if f >= cut]
fr = np.array([f for f, _ in keep], float)
st = np.array([s for _, s in keep], float)
print("onsets:", len(fr), " rate:", t.bpm)
gidx = np.arange(t.cand_count)
gidx = gidx[(gidx % 2) == 0]
bpms, scores, phases, pick = t._score_the(fr, st, gidx)
j = int(np.argmax(pick))
mx = float(np.max(scores))
tol = mx * 1e-4
a = j
while a > 0 and scores[a - 1] >= mx - tol:
    a -= 1
b = j
while b < len(scores) - 1 and scores[b + 1] >= mx - tol:
    b += 1
print("argmax %7.2f  plateau %7.2f..%7.2f  centre %7.2f"
      % (bpms[j], bpms[a], bpms[b], 0.5 * (bpms[a] + bpms[b])))
for k in range(0, len(bpms)):
    if scores[k] > 0.45:
        print("   %7.2f  %.4f" % (bpms[k], scores[k]))

#!/usr/bin/env python3
"""Trace: the published rate, second by second, for a synthetic case."""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _fw_bg as B                                   # noqa: E402
import _fw_port as P                                 # noqa: E402

case = sys.argv[1] if len(sys.argv) > 1 else "breakdown"

if case == "breakdown":
    odf = B.beat_odf([(128.0, 0.0, 20.0, 1.0),
                      (128.0, 25.0, 20.0, 1.0)], dur=45.0)
elif case == "twitch":
    odf = B.beat_odf([(128.0, 0.0, 15.0, 1.0),
                      (127.0, 15.0, 3.0, 1.0),
                      (128.0, 18.0, 15.0, 1.0)], dur=33.0)
elif case == "change":
    odf = B.beat_odf([(128.0, 0.0, 20.0, 1.0),
                      (130.0, 20.0, 20.0, 1.0)], dur=40.0)
else:
    raise SystemExit("no such case")

t = B.BgTracker()
n = len(odf)
i = 0
last = -1
while i < n:
    t.frame(float(odf[i]))
    s = i // P.RATE
    if s != last:
        last = s
        print("  t=%5.1f  rate %8.2f  score %.3f  bg %8.2f sc %.3f loc %.3f "
              "agree %2d conv %d"
              % (s, t.bpm, t.score, t.bg_bpm, t.bg_score, t.bg_local_score,
                 t.bg_agree, int(t.bg_converged)))
    i += 1
print("nudge %d change %d held %d refused %d"
      % (t.nudges, t.changes, t.held_frames, t.bg_refused))

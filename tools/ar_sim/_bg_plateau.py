#!/usr/bin/env python3
"""The plateau's centre over CANDIDATES: is it a better rate than the argmax?"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _fw_bg as B                                   # noqa: E402
import _fw_port as P                                 # noqa: E402


def plateau_centre(scores, bpms, tol=1e-5):
    j = int(np.argmax(scores))
    mx = scores[j]
    on = scores >= mx - max(1e-12, mx * tol)
    a = j
    while a > 0 and on[a - 1]:
        a -= 1
    b = j
    while b < len(on) - 1 and on[b + 1]:
        b += 1
    return 0.5 * (bpms[a] + bpms[b]), (bpms[a], bpms[b]), int(on.sum())


def profile(t, tag, truth):
    cut = t.frame_index - t.bg_window
    keep = [(f, s) for (f, s) in t.peak_frame if f >= cut]
    fr = np.array([f for f, _ in keep], float)
    st = np.array([s for _, s in keep], float)
    bpms = np.arange(115.0, 145.01, 0.25)
    sc, ph = t.comb_scores_all(fr, st, bpms)
    c, (lo, hi), n = plateau_centre(sc, bpms)
    j = int(np.argmax(sc))
    print("  %-10s argmax %7.2f   plateau %5.2f..%5.2f centre %7.2f  "
          "(%d wide)  truth %6.2f   err %5.2f"
          % (tag, bpms[j], lo, hi, c, n, truth, c - truth))


def run(odf, tag, truth, at):
    t = B.BgTracker()
    for i in range(int(at * P.RATE)):
        t.frame(float(odf[i]))
    profile(t, tag, truth)


pure = B.beat_odf([(128.0, 0.0, 40.0, 1.0)], dur=40.0)
mix = B.beat_odf([(128.0, 0.0, 15.0, 1.0),
                  (127.0, 15.0, 3.0, 1.0),
                  (128.0, 18.0, 15.0, 1.0)], dur=33.0)
chg = B.beat_odf([(128.0, 0.0, 20.0, 1.0),
                  (130.0, 20.0, 20.0, 1.0)], dur=40.0)

print("the pure 128 track:")
for at in (6, 10, 16, 25):
    run(pure, "t=%d" % at, 128.0, at)
print("the twitch (the truth is 128, the window holds 3 s of 127):")
for at in (16, 17, 18, 19, 22, 25):
    run(mix, "t=%d" % at, 128.0, at)
print("the change to 130:")
for at in (20, 26, 32, 38):
    run(chg, "t=%d" % at, 130.0 if at > 26 else 128.0, at)

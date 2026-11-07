#!/usr/bin/env python3
"""The SHAPE of the comb's score around its peak, on real music.

The plateau's centre is the tracker's rate.  If the score really is flat
over a wide band, the centre is a biased, jittery answer - so this asks:
how wide is the flat top, in reality?

    python tools/ar_sim/_bg_shape.py
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _fw_bg as B                                     # noqa: E402
import _fw_port as P                                   # noqa: E402
import _cache                                          # noqa: E402


def main():
    keys = _cache.keys()[:12]
    for k in keys:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.odf_of(x)
        if len(odf) < 1200:
            continue
        t = B.BgTracker()
        t.run(odf)
        if not t.locked:
            continue
        # The narrow window's own scores, at the clock's rate.
        bpms = np.arange(t.bpm - 2.0, t.bpm + 2.0 + 1e-9, 0.25)
        keep = [(f, s) for (f, s) in t.peak_frame
                if f >= t.frame_index - P.COMB_FRAMES]
        frames = np.array([f for f, _ in keep], dtype=float)
        strs = np.array([s for _, s in keep], dtype=float)
        if len(frames) < 8:
            continue
        scores, phases = t.comb_scores_all(frames, strs, bpms)
        j = int(np.argmax(scores))
        s = scores[j]
        tol = s * 1e-4
        a = j
        while a > 0 and scores[a - 1] >= s - tol:
            a -= 1
        b = j
        while b < len(scores) - 1 and scores[b + 1] >= s - tol:
            b += 1
        centre = 0.5 * (bpms[a] + bpms[b])
        print("%-14s clock %7.2f  peak %7.2f (score %.4f)  plateau "
              "[%.2f..%.2f] w %.2f  centre %7.3f  local %.4f"
              % (k[:14], t.bpm, bpms[j], s, bpms[a], bpms[b],
                 bpms[b] - bpms[a], centre,
                 float(t.comb_score(frames, strs, t.bpm)[0])))
        print("     scores: %s" % " ".join("%.4f" % v for v in scores))


if __name__ == "__main__":
    sys.exit(main())

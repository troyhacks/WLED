#!/usr/bin/env python3
"""HOW THE MARGIN EVOLVES, FRAME BY FRAME.

WHY
---
The veto - "the far rate may only move the grid if it is clearly better"
- works on the corpus, and it BREAKS the re-lock.  The two cases overlap:
an unchanged track can have a far rate 0.085 better, and a changed one
only 0.043 better.

But the margin is not a constant.  The comb looks at TWELVE SECONDS of
onsets, and right after the music changes that window is still full of the
OLD music: the old rate still scores well and the new rate scores badly.
As the window turns over - a second, two, twelve - the old rate's score
falls and the new one's rises, so the margin GROWS.

So the margin is traced, frame by frame, for both cases:

  UNCHANGED   the margin over the whole window.  The veto's threshold has
              to sit above this, or the grid is dragged for ever.
  CHANGED     the margin after the cut.  The threshold has to be crossed
              in reasonable time, or the tracker never re-locks.

    python tools/ar_sim/_fw_margevo.py --windows 30 --pairs 10
"""

import argparse
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402

ALL_BPMS = P.BPM_LO + np.arange(P.CAND_COUNT) * P.CAND_STEP
NARROW = 2.0


def margin_at(t, frame, rate):
    """(far bpm, far score, local bpm, local score, margin) on one snapshot."""
    cutoff = frame - P.COMB_FRAMES
    keep = [(f, s) for (f, s) in t.peak_frame if f >= cutoff]
    if len(keep) < P.MIN_PEAKS:
        return None
    frames = np.array([f for f, _ in keep], dtype=float)
    strs = np.array([s for _, s in keep], dtype=float)
    scores, _ph = t.comb_scores_all(frames, strs, ALL_BPMS)
    values = scores - t.prior
    lo = max(P.BPM_LO, rate - NARROW)
    hi = min(P.BPM_HI, rate + NARROW)
    f0 = int(np.ceil((lo - P.BPM_LO) / P.CAND_STEP))
    c0 = int(np.floor((hi - P.BPM_LO) / P.CAND_STEP)) - f0 + 1
    idx = np.arange(f0, f0 + c0)
    jn = int(idx[np.argmax(values[idx])])
    jg = int(np.argmax(values))
    if f0 <= jg <= f0 + c0 - 1:
        return None                    # the far rate IS the local rate
    return (float(ALL_BPMS[jg]), float(scores[jg]),
            float(ALL_BPMS[jn]), float(scores[jn]),
            float(scores[jg] - scores[jn]))


def tracker():
    t = P.Tracker()
    t.narrow_bpm = NARROW
    t.search_every = 4
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = 32
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=30)
    ap.add_argument("--pairs", type=int, default=10)
    ap.add_argument("--lo", type=float, default=80.0)
    ap.add_argument("--hi", type=float, default=320.0)
    args = ap.parse_args()

    t0 = time.time()
    items = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, args.lo, args.hi)
        if len(odf) < 1200:
            continue
        items.append((k, odf))
    print("%d windows   %.0f s" % (len(items), time.time() - t0), flush=True)

    # ---- unchanged: the margin over the whole window --------------------
    print()
    print("UNCHANGED: the margin at every net frame")
    worst = []
    for (k, odf) in items:
        t = tracker()
        t.run(odf)
        if not t.locked:
            continue
        # re-run the frame loop, sampling the margin every 32 frames
        t2 = tracker()
        log = []
        for i in range(len(odf)):
            t2.frame(float(odf[i]))
            if t2.frame_index % 32 == 0 and t2.locked:
                m = margin_at(t2, t2.frame_index, t2.bpm)
                if m is not None:
                    log.append((t2.frame_index, m[0], m[1], m[2], m[3], m[4]))
        if not log:
            continue
        mx = max(log, key=lambda r: r[5])
        worst.append(mx[5])
        if len(worst) <= 10:
            print("  %-38s  max margin %+.3f  (far %6.2f %.3f / local %6.2f"
                  " %.3f)  %d far frames"
                  % (k[:38], mx[5], mx[1], mx[2], mx[3], mx[4], len(log)))
    if worst:
        a = np.array(worst)
        print()
        print("  the WORST margin per window:  median %+.3f  p90 %+.3f  max"
              " %+.3f" % (float(np.median(a)), float(np.percentile(a, 90)),
                          float(a.max())))

    # ---- changed: the margin after the cut ------------------------------
    print()
    print("CHANGED: the margin after the cut")
    for i in range(len(items)):
        for j in range(i + 1, len(items)):
            odf_a, odf_b = items[i][1], items[j][1]
            t = tracker()
            cut = len(odf_a)
            row = []
            t.run(np.concatenate((odf_a, odf_b)))
            # the trace, by re-running and sampling
            t2 = tracker()
            both = np.concatenate((odf_a, odf_b))
            for k2 in range(len(both)):
                t2.frame(float(both[k2]))
                if k2 >= cut and (k2 - cut) % 32 == 0 and t2.locked:
                    m = margin_at(t2, t2.frame_index, t2.bpm)
                    if m is not None:
                        row.append(((k2 - cut) / P.RATE, m[0], m[1], m[2], m[3],
                                    m[4]))
            if row:
                print("  %-30s -> %-30s" % (items[i][0][:30], items[j][0][:30]))
                for r in row[:26]:
                    print("      +%5.1f s  far %6.2f %.3f  local %6.2f %.3f"
                          "  margin %+.3f" % (r[0], r[1], r[2], r[3], r[4], r[5]))
            break
        if i >= 5:
            break

    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

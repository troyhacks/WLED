#!/usr/bin/env python3
"""HOW MUCH BETTER MUST A FAR-AWAY RATE BE BEFORE IT MAY MOVE THE GRID?

THE QUESTION
------------
The whole-band safety net finds the best rate in the whole band.  The
narrow search finds the best rate in a couple of BPM around the grid.
They disagree often, and when they do, the grid is dragged between them -
which is what the board was doing, answering 125.00 and then 131.00.

The fix has to tell the two cases apart:

  THE MUSIC CHANGED   the narrow search is looking in the wrong place, so
                      its score COLLAPSES and the whole band's rate is much
                      better evidence.  The grid must move.

  THE MUSIC DID NOT   two comb peaks are nearly equal and the narrow one
                      is not really better.  The grid must NOT move.

So the margin is measured: the whole band's best RAW score minus the
narrow window's best RAW score, on the SAME onset snapshot.

The PRIOR is not evidence.  The prior is a tie-breaker, so the comparison
is on the raw score - otherwise the prior would decide the fight, which
is exactly what it was doing.

    python tools/ar_sim/_fw_margin.py --windows 40
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


def snapshot(t, frame):
    """The onsets in the comb's window at `frame`, as the C++ snapshots them."""
    cutoff = frame - P.COMB_FRAMES
    keep = [(f, s) for (f, s) in t.peak_frame if f >= cutoff]
    if len(keep) < P.MIN_PEAKS:
        return None, None
    return (np.array([f for f, _ in keep], dtype=float),
            np.array([s for _, s in keep], dtype=float))


def margin_at(t, frame, rate):
    """(global bpm, global score, narrow bpm, narrow score, margin).

    The two scores are measured on the SAME snapshot, so they are
    comparable.
    """
    frames, strs = snapshot(t, frame)
    if frames is None:
        return None
    scores, _phases = t.comb_scores_all(frames, strs, ALL_BPMS)
    values = scores - t.prior
    jg = int(np.argmax(values))
    lo = max(P.BPM_LO, rate - 2.0)
    hi = min(P.BPM_HI, rate + 2.0)
    mask = (ALL_BPMS >= lo - 1e-6) & (ALL_BPMS <= hi + 1e-6)
    if not mask.any():
        return None
    idx = np.where(mask)[0]
    jn = int(idx[np.argmax(values[idx])])
    return (float(ALL_BPMS[jg]), float(scores[jg]),
            float(ALL_BPMS[jn]), float(scores[jn]),
            float(scores[jg] - scores[jn]))


def pct(a, p):
    return float(np.percentile(np.array(a), p)) if len(a) else float("nan")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=40)
    ap.add_argument("--pairs", type=int, default=16)
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
    print("%d windows   %.0f s" % (len(items), time.time() - t0))
    print()

    # ---- the music did NOT change -------------------------------------
    same_margins = []
    same_far = 0
    same_n = 0
    for (k, odf) in items:
        t = P.Tracker()
        t.narrow_bpm = 2.0
        t.collapse_trigger = True
        t.full_every = 32
        t.run(odf)
        if not t.locked or t.bpm <= 0:
            continue
        m = margin_at(t, t.frame_index, t.bpm)
        if m is None:
            continue
        gb, gs, nb, ns, mg = m
        same_n += 1
        if abs(gb - nb) > 2.0:
            same_far += 1
            same_margins.append(mg)
        if len(same_margins) <= 8:
            print("  same: %-38s grid %6.2f  far %6.2f (%.3f)  local %6.2f (%.3f)"
                  "  margin %+.3f" % (k[:38], t.bpm, gb, gs, nb, ns, mg))

    # ---- the music DID change ------------------------------------------
    changed_margins = []
    changed_far = 0
    changed_n = 0
    for i in range(len(items) - 1):
        for j in range(i + 1, len(items)):
            odf_a, odf_b = items[i][1], items[j][1]
            t = P.Tracker()
            t.narrow_bpm = 2.0
            t.collapse_trigger = True
            t.full_every = 32
            t.run(np.concatenate((odf_a, odf_b)))
            cut = len(odf_a)
            # the grid's rate just before the cut
            log = np.array(t.bpm_log)
            pre = log[:cut]
            pre = pre[pre > 0]
            if len(pre) < 50:
                continue
            rate = float(pre[-1])
            m = margin_at(t, cut, rate)
            if m is None:
                continue
            gb, gs, nb, ns, mg = m
            changed_n += 1
            if abs(gb - rate) > 2.0:
                changed_far += 1
                changed_margins.append(mg)
            if len(changed_margins) <= 8:
                print("  chg : %-38s grid %6.2f  far %6.2f (%.3f)  local %6.2f"
                      " (%.3f)  margin %+.3f" % (items[i][0][:38], rate, gb, gs,
                                                nb, ns, mg))
            break
        if changed_n >= args.pairs:
            break

    print()
    print("THE MUSIC DID NOT CHANGE: %d windows, %d with the far rate outside"
          " the window" % (same_n, same_far))
    if same_margins:
        a = np.array(same_margins)
        print("  margin   min %+.3f  p10 %+.3f  median %+.3f  p90 %+.3f  max"
              " %+.3f" % (a.min(), pct(a, 10), np.median(a), pct(a, 90), a.max()))
    print()
    print("THE MUSIC DID CHANGE: %d cuts, %d with the far rate outside the"
          " window" % (changed_n, changed_far))
    if changed_margins:
        a = np.array(changed_margins)
        print("  margin   min %+.3f  p10 %+.3f  median %+.3f  p90 %+.3f  max"
              " %+.3f" % (a.min(), pct(a, 10), np.median(a), pct(a, 90), a.max()))
    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

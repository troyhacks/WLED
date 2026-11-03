#!/usr/bin/env python3
"""THE SCORE'S DYNAMIC RANGE, AND WHAT IT DOES AT A TRACK CHANGE.

Two questions, and they are the same question:

  1. How far apart are the score at the RIGHT rate and the score at a rate
     that is simply wrong?  That is the dynamic range, and MIN_SCORE has to
     live inside it - below the wrong rate's score (so it never fires on
     good music) and above nothing (so it CAN fire).

  2. What does the score do when the music REALLY changes - two tracks
     with different tempos, cut against each other?

The first attempt at (2) was wrong: it cut a track against ITSELF, sixty
seconds later, so the tempo never changed and of course the score never
collapsed.  The pairs here are DIFFERENT tracks, and their tempos are
known to be at least 6 BPM apart.

    python tools/ar_sim/_fw_cutscore2.py --windows 48 --pairs 14
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

THRESHOLDS = [0.05, 0.10, 0.20, 0.25, 0.30, 0.35, 0.40, 0.45, 0.50]


def make(margin=0.70, gain=0.10, prior=0.6, narrow_prior=True):
    t = P.Tracker()
    t.narrow_bpm = 2.0
    t.search_every = 4
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = 32
    t.net_margin = margin
    t.pll_gain = gain
    t.prior_weight = prior
    t.prior = t._prior_table()
    t.prior_in_narrow = narrow_prior
    return t


def comb_score(t, frames, strengths, bpm):
    s, _ph = t.comb_scores_all(frames, strengths, np.array([bpm]))
    return float(s[0])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=48)
    ap.add_argument("--pairs", type=int, default=14)
    ap.add_argument("--lo", type=float, default=80.0)
    ap.add_argument("--hi", type=float, default=320.0)
    ap.add_argument("--margin", type=float, default=0.70)
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

    # ---- 1. the dynamic range --------------------------------------------
    #
    # The score at the rate the tracker settled on, and the score at rates
    # that are simply wrong: the half, the double, and a rate a long way
    # off.  The gap between them is what MIN_SCORE can work with.
    print()
    print("THE DYNAMIC RANGE  (the score at the right rate, and at wrong ones)")
    right, half_, double_, far_ = [], [], [], []
    for (k, odf) in items:
        t = make(args.margin)
        t.run(odf)
        if not t.locked or t.bpm <= 0.0:
            continue
        log = np.array(t.bpm_log)
        log = log[log > 0]
        if len(log) < 100:
            continue
        bpm = float(np.median(log))
        # the snapshot: the whole window, as the tracker sees it
        cutoff = t.frame_index - P.COMB_FRAMES
        keep = [(f, s) for (f, s) in t.peak_frame if f >= cutoff]
        if len(keep) < P.MIN_PEAKS:
            continue
        fr = np.array([f for f, _ in keep], dtype=float)
        st = np.array([s for _, s in keep], dtype=float)
        right.append(comb_score(t, fr, st, bpm))
        half_.append(comb_score(t, fr, st, bpm * 0.5))
        double_.append(comb_score(t, fr, st, bpm * 2.0))
        far_.append(comb_score(t, fr, st, bpm * 1.37))
    if right:
        r = np.array(right)
        h = np.array(half_)
        d = np.array(double_)
        fz = np.array(far_)
        print("   the right rate    min %.3f  median %.3f  max %.3f"
              % (r.min(), np.median(r), r.max()))
        print("   at half of it     min %.3f  median %.3f  max %.3f"
              % (h.min(), np.median(h), h.max()))
        print("   at twice of it    min %.3f  median %.3f  max %.3f"
              % (d.min(), np.median(d), d.max()))
        print("   at 1.37x of it   min %.3f  median %.3f  max %.3f"
              % (fz.min(), np.median(fz), fz.max()))
        print("   the right rate is better than the wrong ones in %.0f %% of"
              % (100.0 * np.mean((r > h) & (r > d) & (r > fz))))
        print("   the windows, and the gap between the right and the wrong:")
        print("      median gap to the best wrong one   %.3f"
              % float(np.median(r - np.maximum(np.maximum(h, d), fz))))
        print("      smallest gap in any window           %.3f"
              % float(np.min(r - np.maximum(np.maximum(h, d), fz))))
    print()

    # ---- 2. the track change ---------------------------------------------
    #
    # DIFFERENT tracks, and their tempos at least 6 BPM apart.  The
    # reference for B is the rate a tracker finds when it is given B alone.
    def reference(odf):
        t = P.Tracker()
        t.narrow_bpm = 0.0
        t.run(odf)
        if not t.locked or t.bpm <= 0.0:
            return None
        return t.bpm

    refs = {}
    for (k, odf) in items:
        refs[k] = reference(odf)
    pairs = []
    for i in range(len(items)):
        for j in range(len(items)):
            if items[j][0].split(" @")[0] == items[i][0].split(" @")[0]:
                continue
            if refs.get(items[j][0]) is None or refs.get(items[i][0]) is None:
                continue
            if abs(refs[items[j][0]] - refs[items[i][0]]) < 6.0:
                continue
            pairs.append((items[i], items[j]))
            break
        if len(pairs) >= args.pairs:
            break
    print("%d pairs of DIFFERENT tracks, at least 6 BPM apart" % len(pairs))
    print()

    floors = []
    fired = {th: [] for th in THRESHOLDS}
    never = {th: 0 for th in THRESHOLDS}
    relocks = []
    for ((ka, odf_a), (kb, odf_b)) in pairs:
        cut = len(odf_a)
        t = make(args.margin)
        t.run(np.concatenate((odf_a, odf_b)))
        sc = np.array(t.score_log)
        if len(sc) < cut + 10:
            continue
        after = sc[cut:]
        floors.append(float(after.min()))
        for th in THRESHOLDS:
            hit = np.nonzero(after < th)[0]
            if len(hit):
                fired[th].append(float(hit[0]) / P.RATE)
            else:
                never[th] += 1
        # the re-lock: the first moment the published rate is on B's rate
        # and stays there for three seconds
        blog = np.array(t.bpm_log)
        rb = refs[kb]
        first = None
        for f in range(cut, len(blog)):
            if t.locked and abs(blog[f] - rb) <= 1.0:
                first = f
                break
        if first is None:
            relocks.append(999.0)
        else:
            ok = all(t.locked and abs(blog[f] - rb) <= 2.0
                     for f in range(first, min(len(blog), first + 281)))
            relocks.append((first - cut) / P.RATE if ok else 999.0)
    if not floors:
        print("no usable pairs")
        return 1
    f = np.array(floors)
    print("the score's floor AFTER the cut")
    print("   min %.3f   median %.3f   max %.3f" % (f.min(), np.median(f), f.max()))
    print()
    print("how long after the cut the score falls under a threshold")
    print("   %-10s %10s %10s %8s" % ("threshold", "median s", "mean s", "never"))
    for th in THRESHOLDS:
        a = np.array(fired[th]) if fired[th] else np.array([0.0])
        print("   %-10.2f %10.2f %10.2f %8d"
              % (th, float(np.median(a)), float(a.mean()), never[th]))
    r = np.array(relocks)
    print()
    print("the re-lock: median %.2f s   never %d of %d"
          % (float(np.median(r)), int(np.sum(r > 900.0)), len(r)))
    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Does the veto's kernel difference inflate the far candidate's score?

The veto compares the far candidate's RAW score against the narrow
candidate's RAW score.  Both are scored by arTempoCombScore(), which uses
arTempoKernelHalfWidth(period) - an INTEGER bin count that depends on the
candidate's period.  So the two scores are measured with different kernels.

This measures how much of the far candidate's advantage is the kernel.

    python tools/ar_sim/_diag_kernel.py --windows 30
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


def kernel_w(period):
    """arTempoKernelHalfWidth()."""
    tol_frames = (P.TOL_MS * 0.001) * P.RATE
    w = int(np.rint(tol_frames / period * P.COMB_BINS))
    return max(1, min(w, P.COMB_BINS // 2 - 1))


def score_with_w(frames, strengths, bpm, w):
    """arTempoCombScore() but with the kernel width forced."""
    n = len(frames)
    total = float(np.sum(strengths))
    if total <= 0 or n == 0:
        return 0.0
    period = 60.0 * P.RATE / bpm
    u = np.mod(frames, period)
    bins = np.clip(np.floor(u / period * P.COMB_BINS).astype(int),
                   0, P.COMB_BINS - 1)
    h = np.bincount(bins, weights=strengths, minlength=P.COMB_BINS)
    h3 = np.concatenate((h, h, h))
    pre = np.concatenate((np.zeros(1), np.cumsum(h3)))
    i = np.arange(P.COMB_BINS)
    hi = P.COMB_BINS + i + w + 1
    lo = P.COMB_BINS + i - w
    win = pre[hi] - pre[lo]
    return float(win.max()) / total


def snapshot(t, frame):
    cutoff = frame - P.COMB_FRAMES
    keep = [(f, s) for (f, s) in t.peak_frame if f >= cutoff]
    if len(keep) < P.MIN_PEAKS:
        return None, None
    return (np.array([f for f, _ in keep], dtype=float),
            np.array([s for _, s in keep], dtype=float))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=30)
    ap.add_argument("--lo", type=float, default=80.0)
    ap.add_argument("--hi", type=float, default=320.0)
    ap.add_argument("--margin", type=float, default=0.20)
    args = ap.parse_args()

    items = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, args.lo, args.hi)
        if len(odf) < 1200:
            continue
        items.append((k, odf))
    print("%d windows" % len(items))
    print()
    print("  %-34s %6s %6s | %6s %6s %6s | %6s %6s | %s"
          % ("track", "near", "far", "wnear", "wfar", "ratio", "own",
             "common", "flip"))
    flips = 0
    n_far = 0
    infl = []
    for (k, odf) in items:
        t = P.Tracker()
        t.narrow_bpm = 2.0
        t.collapse_trigger = True
        t.full_every = 32
        t.net_margin = args.margin
        t.run(odf)
        if not t.locked or t.bpm <= 0:
            continue
        frames, strs = snapshot(t, t.frame_index)
        if frames is None:
            continue
        scores, _ph = t.comb_scores_all(frames, strs, ALL_BPMS)
        values = scores - t.prior
        jg = int(np.argmax(values))
        lo = max(P.BPM_LO, t.bpm - 2.0)
        hi = min(P.BPM_HI, t.bpm + 2.0)
        idx = np.where((ALL_BPMS >= lo - 1e-6) & (ALL_BPMS <= hi + 1e-6))[0]
        if len(idx) == 0:
            continue
        jn = int(idx[np.argmax(values[idx])])
        if jg == jn:
            continue
        n_far += 1
        near_bpm = float(ALL_BPMS[jn])
        far_bpm = float(ALL_BPMS[jg])
        p_near = 60.0 * P.RATE / near_bpm
        p_far = 60.0 * P.RATE / far_bpm
        w_near = kernel_w(p_near)
        w_far = kernel_w(p_far)
        near = float(scores[jn])
        far_own = float(scores[jg])
        far_common = score_with_w(frames, strs, far_bpm, w_near)
        r_own = far_own / near if near > 0 else float('nan')
        r_com = far_common / near if near > 0 else float('nan')
        infl.append((far_own - far_common) / far_common)
        own_far_wins = r_own >= (1.0 + args.margin)
        com_far_wins = r_com >= (1.0 + args.margin)
        flip = own_far_wins != com_far_wins
        if flip:
            flips += 1
        print("  %-34s %6.2f %6.2f | %6d %6d %6.3f | %6.3f %6.3f | %s%s"
              % (k[:34], near_bpm, far_bpm, w_near, w_far, r_own,
                 r_own, r_com, "FLIP " if flip else "",
                 "" if com_far_wins else "(common keeps grid)"))
    print()
    if infl:
        a = np.array(infl) * 100.0
        print("kernel-only inflation of the far score: min %+.1f%%  median %+.1f%%"
              "  max %+.1f%%  mean %+.1f%%"
              % (a.min(), float(np.median(a)), a.max(), a.mean()))
    print("%d far-vs-near pairs, %d decision flips from the kernel alone"
          % (n_far, flips))


if __name__ == "__main__":
    sys.exit(main())

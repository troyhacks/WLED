#!/usr/bin/env python3
"""HOW MUCH DID THE float32 PHASE MOVE THE SCORES?

The port used to compute the comb's phase in float64 and the firmware
computes it in float32.  The phase decides which bin an onset lands in, and
an onset that sits within a thousandth of a bin edge lands in a DIFFERENT
bin on the board.  One onset moved between neighbouring bins moves the
score by up to 1.7 %, which is a tenth of the veto's margin - so the
measurements that chose the margin were made with an instrument that was
not the firmware.

This measures the size of the effect on REAL data: the same onsets scored
by both phase rules, and the same onsets scored by both rules with the
argmax taken over the whole band.

    python tools/ar_sim/_fw_f32.py --windows 20
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402

f32 = np.float32


def scores_float64_phase(frames, strengths, bpms):
    """The port's OLD phase: np.mod in float64."""
    n = len(frames)
    total = float(np.sum(strengths))
    c = len(bpms)
    periods = 60.0 * P.RATE / bpms
    u = np.mod(frames[None, :], periods[:, None])
    bins = np.clip(np.floor(u / periods[:, None] * P.COMB_BINS).astype(int),
                   0, P.COMB_BINS - 1)
    flat = (np.repeat(np.arange(c), n) * P.COMB_BINS + bins.ravel())
    h = np.bincount(flat, weights=np.tile(strengths, c),
                    minlength=c * P.COMB_BINS).reshape(c, P.COMB_BINS)
    h3 = np.concatenate((h, h, h), axis=1)
    pre = np.concatenate((np.zeros((c, 1)), np.cumsum(h3, axis=1)), axis=1)
    i = np.arange(P.COMB_BINS)
    wf = np.clip((P.TOL_MS * 0.001) * P.RATE / periods * P.COMB_BINS,
                 1.0, float(P.COMB_BINS // 2 - 2))
    k = np.floor(wf).astype(int)
    f = wf - k
    hi = P.COMB_BINS + i[None, :] + k[:, None] + 1
    lo = P.COMB_BINS + i[None, :] - k[:, None]
    win = (np.take_along_axis(pre, hi, axis=1)
           - np.take_along_axis(pre, lo, axis=1))
    kk = k[:, None]
    ff = f[:, None]
    narrow = (ff < 0.5)
    idx_lo = P.COMB_BINS + i[None, :] - kk
    idx_hi = P.COMB_BINS + i[None, :] + kk
    lo_i = np.where(narrow, idx_lo, idx_lo - 1)
    hi_i = np.where(narrow, idx_hi, idx_hi + 1)
    h_lo = np.take_along_axis(h3, np.clip(lo_i, 0, h3.shape[1] - 1), axis=1)
    h_hi = np.take_along_axis(h3, np.clip(hi_i, 0, h3.shape[1] - 1), axis=1)
    amt = np.where(narrow, 0.5 - ff, ff - 0.5)
    sgn = np.where(narrow, -1.0, 1.0)
    win = win + sgn * amt * (h_lo + h_hi)
    return win.max(axis=1) / total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=20)
    ap.add_argument("--band-lo", type=float, default=80.0)
    ap.add_argument("--band-hi", type=float, default=320.0)
    args = ap.parse_args()

    bpms = P.BPM_LO + np.arange(P.CAND_COUNT) * P.CAND_STEP
    worst = 0.0
    flips = 0
    total_cands = 0
    dscore = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, args.band_lo, args.band_hi)
        if len(odf) < 1200:
            continue
        # the onsets the picker would find
        tr = P.Tracker()
        for v in odf:
            v = float(v)
            tr.frame_index += 1
            if v > tr.odf_max:
                tr.odf_max = v
            else:
                tr.odf_max *= P.MAX_DECAY
            tr.pick_peak(v, tr.frame_index)
        frames = np.array([f for f, _ in tr.peak_frame], dtype=float)
        strs = np.array([s for _, s in tr.peak_frame], dtype=float)
        if len(frames) < 8:
            continue
        a = scores_float64_phase(frames, strs, bpms)
        b, _ = tr.comb_scores_all(frames, strs, bpms)
        d = np.abs(a - b)
        rel = d / np.maximum(np.abs(b), 1e-12)
        worst = max(worst, float(rel.max()))
        dscore.append(float(d.mean()))
        fa = int(np.argmax(a))
        fb = int(np.argmax(b))
        if fa != fb:
            flips += 1
        total_cands += len(bpms)
        print("%-34s  n=%3d  mean |d| %.2e  max rel %.2e  argmax %s"
              % (k[:34], len(frames), d.mean(), rel.max(),
                 ("same" if fa == fb else
                  "%.2f -> %.2f" % (bpms[fa], bpms[fb]))), flush=True)
    print()
    print("the float64 phase vs the float32 one, over %d windows:"
          % len(dscore))
    print("  worst RELATIVE score difference   %.4f %%" % (100.0 * worst))
    print("  mean absolute score difference    %.2e" % (np.mean(dscore)))
    print("  the band's argmax moved on %d of %d windows"
          % (flips, len(dscore)))


if __name__ == "__main__":
    sys.exit(main())

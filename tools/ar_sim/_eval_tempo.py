#!/usr/bin/env python3
"""One comparison, three rate estimators, one metric.

THE METRIC
----------
A constant phase offset is a separate thing from a wrong RATE, and it
does not matter for a strobe: the flash would be consistently early or
late by the same amount every beat, which nobody sees.  A wrong RATE
does matter: the grid drifts relative to the music, so the flash wanders
away from the beat and the effect falls apart.

So the metric is: lay the beat grid at the candidate rate over the
window, slide it by up to +/- 150 ms to remove the constant offset, and
count the fraction of the raw onsets it then lands on.  A right rate
holds that fraction for the whole window; a wrong rate loses it as the
grid drifts.

The onsets come from the raw audio - the 40-160 Hz band's short-term
energy rise - so no beat tracker is acting as the referee, and no
detector's phase convention can bias the result.

THE THREE
---------
  acf+dp   the autocorrelation's peak, refined by the DP beat tracker
  comb     the phase-locked comb score over the onset phases
  oracle   the best rate a brute-force sweep can find - the bar to clear

    python tools/ar_sim/_eval_tempo.py --limit 100
"""

import argparse
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _tempo_acf as T                             # noqa: E402
import _beat as B                                  # noqa: E402
import _comb as C                                  # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402


def hit_with_offset(bpm, beats, onsets, span=0.15, step=0.005):
    """The best hit rate over a constant offset, and that offset."""
    if not len(beats) or not len(onsets):
        return 0.0, 0.0
    best = (0.0, 0.0)
    for off in np.arange(-span, span + 1e-9, step):
        d = np.abs(onsets[:, None] - (beats + off)[None, :]).min(axis=1)
        f = float((d <= 0.070).mean())
        if f > best[0]:
            best = (f, float(off))
    return best


def grid_at(bpm, phase_frames, t_lo, t_hi):
    period = 60.0 * T.RATE / bpm
    k0 = int(np.floor((t_lo - phase_frames) / period))
    k1 = int(np.ceil((t_hi - phase_frames) / period))
    g = phase_frames + np.arange(k0, k1 + 1) * period
    return g[(g >= t_lo) & (g <= t_hi)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=100)
    ap.add_argument("--design", default="hp40_lp160x2")
    ap.add_argument("--whiten", type=float, default=0.0)
    ap.add_argument("--step", type=float, default=0.25)
    ap.add_argument("--weight", type=float, default=0.25)
    ap.add_argument("--prior", type=float, default=125.0)
    ap.add_argument("--oracle", action="store_true",
                    help="also brute-force the best rate (slow)")
    ap.add_argument("--refine", type=float, default=0.0,
                    help="parabolic refinement of the comb score's peak, BPM")
    args = ap.parse_args()

    keys = _cache.keys()[:args.limit]
    win = 1125
    res = {"acf+dp": [], "comb": []}
    offs = {"acf+dp": [], "comb": []}
    rows = []
    t0 = time.time()
    for k in keys:
        x = _cache.load(k)
        if x is None:
            continue
        odf = T.odf_cached(k, x, args.design, args.whiten)
        mx = float(np.max(odf))
        odfn = odf / mx if mx > 0 else odf
        peaks, strengths = B.peak_pick(odfn)
        first = max(0, len(odfn) - win)
        keep = peaks >= first
        peaks = peaks[keep]
        strengths = strengths[keep]
        if len(peaks) < 8:
            continue
        t_lo = first / T.RATE
        t_hi = len(odfn) / T.RATE
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        wo = ons[(ons >= t_lo - 0.02) & (ons <= t_hi)]
        if len(wo) < 4:
            continue

        # 1. the autocorrelation + DP
        bpm_dp, _c, _d = B.estimate_dp(odf, win=win)
        if bpm_dp is not None:
            g = grid_at(bpm_dp, 0.0, first, len(odfn))
            # the DP's phase: the first beat it found
            path, _m = B.dp_beat_track(peaks, strengths,
                                       60.0 * T.RATE / bpm_dp)
            if len(path):
                g = grid_at(bpm_dp, float(peaks[path][0]), first, len(odfn))
            f, off = hit_with_offset(bpm_dp, g / T.RATE, wo)
            res["acf+dp"].append(f)
            offs["acf+dp"].append(off)
        else:
            bpm_dp = None

        # 2. the phase-locked comb
        bpm_c, score_c, phase_c, _d = C.comb_rate(peaks, strengths,
                                                  step=args.step,
                                                  prior_bpm=args.prior,
                                                  prior_weight=args.weight,
                                                  refine=args.refine)
        g = grid_at(bpm_c, phase_c, first, len(odfn))
        f, off = hit_with_offset(bpm_c, g / T.RATE, wo)
        res["comb"].append(f)
        offs["comb"].append(off)

        row = [k, bpm_dp, bpm_c, score_c,
               res["acf+dp"][-1] if bpm_dp is not None else 0.0,
               res["comb"][-1]]
        if args.oracle:
            best_f = -1.0
            best_b = None
            for cand in np.arange(90.0, 175.0, 0.5):
                gg = grid_at(cand, 0.0, first, len(odfn))
                ff, _o = hit_with_offset(cand, gg / T.RATE, wo)
                if ff > best_f:
                    best_f = ff
                    best_b = float(cand)
            row += [best_b, best_f]
        rows.append(row)

    print("%d windows   %.0f s" % (len(rows), time.time() - t0))
    print()
    print("hit rate of the beat grid on the raw onsets, with a constant")
    print("offset removed (so this measures the RATE, not the phase)")
    print()
    for name in ("acf+dp", "comb"):
        a = np.array(res[name])
        o = np.array(offs[name])
        print("  %-8s median %5.3f   >=0.5: %5.1f%%   >=0.8: %5.1f%%   "
              "offset median %+6.1f ms"
              % (name, float(np.median(a)), 100.0 * float((a >= 0.5).mean()),
                 100.0 * float((a >= 0.8).mean()),
                 1000.0 * float(np.median(o))))
    if args.oracle:
        o = np.array([r[6] for r in rows])
        print("  %-8s median %5.3f   >=0.5: %5.1f%%   >=0.8: %5.1f%%"
              % ("oracle", float(np.median(o)), 100.0 * float((o >= 0.5).mean()),
                 100.0 * float((o >= 0.8).mean())))
    print()
    print("where the comb loses to the acf+dp:")
    n = 0
    for r in rows:
        if r[1] is None:
            continue
        if r[5] < r[4] - 0.10:
            n += 1
            if n <= 12:
                print("  %-42s acf+dp %7.2f (hit %4.2f)   comb %7.2f "
                      "(hit %4.2f, score %4.2f)"
                      % (r[0][:42], r[1], r[4], r[2], r[5], r[3]))
    print("  %d of %d windows" % (n, len(rows)))


if __name__ == "__main__":
    sys.exit(main())

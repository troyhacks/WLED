#!/usr/bin/env python3
"""Is the remaining error in the RATE, or in the rate SELECTION?

The regular grid with a phase-locked phase scores a median hit rate of
0.750 and clears 0.5 on 72% of the windows.  The failures could be:

  (a) the rate is wrong - the estimator locked onto a spurious period; or
  (b) the rate is right but the SELECTION picked a worse candidate than
      the ACF's list contained.

This separates them by brute force: for every window it sweeps the BPM
over a fine grid, takes the best-phase regular grid's hit rate for each
BPM, and finds the BPM that scores highest.  That is the ORACLE rate -
the best the architecture could do if the rate selection were perfect.

  our rate's hit rate   vs   the oracle rate's hit rate

If the oracle is much better, the selection is the thing to fix.  If
they are close, the rate is right and the limit is the onset detector.

    python tools/ar_sim/_diag_rate.py --limit 120
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _tempo_acf as T                             # noqa: E402
import _beat as B                                  # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402
from _diag_grid import hit_frac, best_phase_grid   # noqa: E402


def score_bpm(period_s, onsets, t_lo, t_hi):
    grid, f = best_phase_grid(period_s, onsets, t_lo, t_hi)
    return f


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=120)
    ap.add_argument("--design", default="hp40_lp160x2")
    ap.add_argument("--whiten", type=float, default=0.0)
    ap.add_argument("--show", type=int, default=14)
    args = ap.parse_args()

    keys = _cache.keys()[:args.limit]
    win = 1125
    ours = []
    oracles = []
    rows = []
    for k in keys:
        x = _cache.load(k)
        if x is None:
            continue
        odf = T.odf_cached(k, x, args.design, args.whiten)
        mx = float(np.max(odf))
        odfn = odf / mx if mx > 0 else odf
        bpm, conf, det = B.estimate_dp(odf, win=win)
        if bpm is None:
            continue
        first = max(0, len(odfn) - win)
        t_lo = first / T.RATE
        t_hi = len(odfn) / T.RATE
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        win_ons = ons[(ons >= t_lo - 0.02) & (ons <= t_hi)]
        if len(win_ons) < 4:
            continue

        f_ours = score_bpm(60.0 / bpm, win_ons, t_lo, t_hi)

        # the oracle: sweep the rate, keep the best
        best_f = -1.0
        best_bpm = None
        for cand in np.arange(90.0, 176.0, 0.25):
            f = score_bpm(60.0 / cand, win_ons, t_lo, t_hi)
            if f > best_f:
                best_f = f
                best_bpm = float(cand)
        ours.append(f_ours)
        oracles.append(best_f)
        rows.append((k, bpm, f_ours, best_bpm, best_f))

    o = np.array(ours)
    orc = np.array(oracles)
    print("%d windows" % len(rows))
    print()
    print("hit rate of the best-phase regular grid")
    print("  our rate    median %5.3f   >=0.5: %5.1f%%   >=0.8: %5.1f%%"
          % (float(np.median(o)), 100.0 * float((o >= 0.5).mean()),
             100.0 * float((o >= 0.8).mean())))
    print("  oracle rate median %5.3f   >=0.5: %5.1f%%   >=0.8: %5.1f%%"
          % (float(np.median(orc)), 100.0 * float((orc >= 0.5).mean()),
             100.0 * float((orc >= 0.8).mean())))
    print()
    print("when the oracle beats our rate by more than 0.15, the rate")
    print("SELECTION is what failed:")
    worse = [r for r in rows if r[4] - r[2] > 0.15]
    print("  %d of %d windows" % (len(worse), len(rows)))
    for k, bpm, f, bbpm, bf in sorted(worse, key=lambda r: r[4] - r[2],
                                      reverse=True)[:args.show]:
        print("    %-40s ours %7.2f (hit %4.2f)   oracle %7.2f (hit %4.2f)"
              % (k[:40], bpm, f, bbpm, bf))


if __name__ == "__main__":
    sys.exit(main())

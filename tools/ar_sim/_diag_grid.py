#!/usr/bin/env python3
"""REGULAR GRID vs DP BEATS - the decisive experiment for the PLL.

The DP places every beat on the strongest onset near where it thinks the
beat is.  On real music that makes the grid JITTER: the gaps come out
0.480 / 0.501 / 0.427 / 0.554 seconds against a true 0.490, because
sometimes the strongest onset is the kick and sometimes it is the
offbeat bass.  The RATE is right; the PHASE wobbles.

But a strobe does not want 24 wobbly beat times.  It wants a REGULAR
grid at the right rate, phase-locked to the music.  That is what a
phase-locked loop is for, and it is the structure this whole feature was
asked for.

This compares three ways of placing the beats, all at the SAME rate:

  dp      the DP's own beat times (wobbly)
  grid    a regular grid at the DP's period, phase taken from the DP's
          first beat (steady, but the phase can be off by a fraction)
  grid+   a regular grid whose phase is chosen to best land on the
          onsets in the window (steady AND right)

against the raw onsets from the audio, with a fair, windowed comparison.

    python tools/ar_sim/_diag_grid.py
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _oracle_librosa                             # noqa: E402
import _tempo_acf as T                             # noqa: E402
import _beat as B                                  # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402

HIT_TOL = 0.070


def hit_frac(beats, onsets):
    """The fraction of ONSETS that have a beat within the tolerance."""
    if not len(beats) or not len(onsets):
        return 0.0
    d = np.abs(onsets[:, None] - beats[None, :]).min(axis=1)
    return float((d <= HIT_TOL).mean())


def best_phase_grid(period, onsets, t_lo, t_hi):
    """A regular grid at `period`, with the phase that best lands on the
    onsets.  The phase search is over ONE period, in 1/64-period steps."""
    if period <= 0 or not len(onsets):
        return np.zeros(0), 0.0
    best = (None, -1.0)
    for ph in np.arange(0.0, period, period / 64.0):
        k0 = int(np.floor((t_lo - ph) / period))
        k1 = int(np.ceil((t_hi - ph) / period))
        g = ph + np.arange(k0, k1 + 1) * period
        g = g[(g >= t_lo) & (g <= t_hi)]
        if not len(g):
            continue
        f = hit_frac(g, onsets)
        if f > best[1]:
            best = (g, f)
    return (best[0] if best[0] is not None else np.zeros(0)), best[1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keys", default=None)
    ap.add_argument("--design", default="hp40_lp160x2")
    ap.add_argument("--whiten", type=float, default=0.0)
    ap.add_argument("--tol", type=float, default=0.20)
    ap.add_argument("--tightness", type=float, default=1.0)
    ap.add_argument("--show", type=int, default=10)
    args = ap.parse_args()

    keys = _cache.keys()
    if args.keys:
        keys = [k for k in keys if args.keys in k]
    win = 1125

    res = {"dp": [], "grid": [], "grid+": [], "lib": []}
    rows = []
    for k in keys:
        x = _cache.load(k)
        o = _oracle_librosa.load(k)
        if x is None or o is None or not o.get("beats"):
            continue
        odf = T.odf_cached(k, x, args.design, args.whiten)
        mx = float(np.max(odf))
        odfn = odf / mx if mx > 0 else odf
        bpm, conf, det = B.estimate_dp(odf, win=win, tol=args.tol,
                                       tightness=args.tightness)
        if bpm is None:
            continue
        period_s = 60.0 / bpm

        peaks, strengths = B.peak_pick(odfn)
        first = max(0, len(odfn) - win)
        keep = peaks >= first
        peaks = peaks[keep]
        strengths = strengths[keep]
        path, _m = B.dp_beat_track(peaks, strengths, 60.0 * T.RATE / bpm,
                                    tol=args.tol, tightness=args.tightness)
        if len(path) < 4:
            continue
        dp_beats = peaks[path] / T.RATE

        # the window, in seconds, that the beats cover
        t_lo = first / T.RATE
        t_hi = len(odfn) / T.RATE

        # the onsets in that window, from the raw audio
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        win_ons = ons[(ons >= t_lo - 0.02) & (ons <= t_hi)]
        if len(win_ons) < 4:
            continue

        # 1. the DP's own beat times
        f_dp = hit_frac(dp_beats, win_ons)

        # 2. a regular grid at the same rate, the DP's phase
        ph0 = dp_beats[0] % period_s
        k0 = int(np.floor((t_lo - ph0) / period_s))
        k1 = int(np.ceil((t_hi - ph0) / period_s))
        grid = ph0 + np.arange(k0, k1 + 1) * period_s
        grid = grid[(grid >= t_lo) & (grid <= t_hi)]
        f_grid = hit_frac(grid, win_ons)

        # 3. a regular grid with the best phase
        grid2, f_grid2 = best_phase_grid(period_s, win_ons, t_lo, t_hi)

        # librosa, over the SAME window, for the bar to clear
        lib = np.asarray(o["beats"], dtype=float)
        libw = lib[(lib >= t_lo) & (lib <= t_hi)]
        f_lib = hit_frac(libw, win_ons)

        res["dp"].append(f_dp)
        res["grid"].append(f_grid)
        res["grid+"].append(f_grid2)
        res["lib"].append(f_lib)
        rows.append((k, bpm, f_dp, f_grid, f_grid2, f_lib, len(dp_beats)))

    print("%d windows" % len(rows))
    print()
    print("the fraction of the raw onsets in the window that a beat lands on")
    print()
    for name in ("lib", "dp", "grid", "grid+"):
        a = np.array(res[name])
        print("  %-6s median %5.3f   mean %5.3f   >=0.5: %5.1f%%   >=0.8: %5.1f%%"
              % (name, float(np.median(a)), float(a.mean()),
                 100.0 * float((a >= 0.5).mean()), 100.0 * float((a >= 0.8).mean())))
    print()
    print("worst for the regular grid with the best phase:")
    rows.sort(key=lambda r: r[4])
    for k, bpm, f_dp, f_g, f_g2, f_lib, nb in rows[:args.show]:
        print("  %-42s bpm %7.2f   dp %5.3f   grid %5.3f   grid+ %5.3f   "
              "librosa %5.3f" % (k[:42], bpm, f_dp, f_g, f_g2, f_lib))


if __name__ == "__main__":
    sys.exit(main())

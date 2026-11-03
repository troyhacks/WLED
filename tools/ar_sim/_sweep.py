#!/usr/bin/env python3
"""Sweep the beat tracker's parameters against a convention-free metric.

The metric is the HIT RATE: lay the estimated beat grid over the raw
kick onsets and count how many onsets have a beat within 70 ms.  It
needs no beat tracker as a reference, so it cannot be fooled by a
convention difference - it asks the only question that matters for a
strobe: does the grid land on the kick?

librosa's beat grid scores a median hit rate of 0.81 on this corpus, so
that is the bar to clear.

    python tools/ar_sim/_sweep.py --limit 80
"""

import argparse
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _oracle_librosa                             # noqa: E402
import _tempo_acf as T                             # noqa: E402
import _beat as B                                  # noqa: E402
from _diag_phase import kick_onsets, best_offset   # noqa: E402


def hit_rate(bpm, beats, onsets, span=0.25, step=0.005):
    """The best hit rate over a small offset search, and that offset."""
    off, hit = best_offset(bpm, beats, onsets, span=span, step=step)
    if off is None:
        return 0.0, 0.0
    return hit, off


def run(keys, design, whiten, win, tol, tightness, prior_bpm, prior_weight,
        top, min_beats=8):
    hits = []
    offs = []
    bpms = []
    libs = []
    for k in keys:
        x = _cache.load(k)
        o = _oracle_librosa.load(k)
        if x is None or o is None or not o.get("beats"):
            continue
        onsets = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        if len(onsets) < 8:
            continue
        odf = T.odf_cached(k, x, design, whiten)
        mx = float(np.max(odf))
        odfn = odf / mx if mx > 0 else odf
        bpm, conf, det = B.estimate_dp(odf, win=win, top=top,
                                       prior_bpm=prior_bpm,
                                       prior_weight=prior_weight,
                                       tol=tol, tightness=tightness,
                                       min_beats=min_beats)
        if bpm is None:
            continue
        peaks, strengths = B.peak_pick(odfn)
        first = max(0, len(odfn) - win)
        keep = peaks >= first
        peaks = peaks[keep]
        strengths = strengths[keep]
        path, _m = B.dp_beat_track(peaks, strengths, 60.0 * T.RATE / bpm,
                                    tol=tol, tightness=tightness)
        if len(path) < 4:
            continue
        beats = peaks[path] / T.RATE
        # the onsets must be the ones in the SAME window the beats are in,
        # or the hit rate is capped by the fraction of the window the
        # beats cover
        t0 = first / T.RATE
        win_onsets = onsets[onsets >= t0 - 0.05]
        if len(win_onsets) < 4:
            continue
        h, off = hit_rate(bpm, beats, win_onsets)
        ibi = np.diff(np.asarray(o["beats"], dtype=float))
        lib_bpm = 60.0 / float(np.median(ibi)) if len(ibi) else o["bpm"]
        hits.append(h)
        offs.append(off)
        bpms.append(bpm)
        libs.append(lib_bpm)
    return (np.array(hits), np.array(offs), np.array(bpms), np.array(libs))


def summarise(label, hits, offs, bpms, libs):
    if not len(hits):
        print("%-46s no rows" % label)
        return
    # the BPM agreement, as a ratio, so an octave error is visible
    rat = bpms / libs
    near = float((np.abs(bpms - libs) <= 3.0).mean())
    oct = float(((rat > 0.485) & (rat < 0.515)).mean()
                + ((rat > 1.94) & (rat < 2.06)).mean())
    print("%-46s hit %5.3f  hit>=0.5 %5.1f%%  bpm ok %5.1f%%  octave %4.1f%%  "
          "off %+6.1f ms"
          % (label, float(np.median(hits)), 100.0 * float((hits >= 0.5).mean()),
             100.0 * near, 100.0 * oct,
             1000.0 * float(np.median(offs))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=80)
    ap.add_argument("--design", default="hp40_lp160x2")
    ap.add_argument("--whiten", type=float, default=0.0)
    ap.add_argument("--window", type=float, default=T.WINDOW_S)
    ap.add_argument("--prior", type=float, default=B.PRIOR_BPM)
    ap.add_argument("--weight", type=float, default=B.PRIOR_WEIGHT)
    ap.add_argument("--top", type=int, default=10)
    args = ap.parse_args()

    keys = _cache.keys()[:args.limit]
    win = int(args.window * T.RATE)
    print("%d windows, design %s, whiten %.2f, prior %.0f BPM weight %.2f"
          % (len(keys), args.design, args.whiten, args.prior, args.weight))
    print()
    print("%-46s %s" % ("config", "result"))
    t0 = time.time()
    for tol in (0.05, 0.08, 0.12, 0.20):
        for tight in (1.0, 3.0, 8.0, 20.0):
            h, o, b, l = run(keys, args.design, args.whiten, win, tol, tight,
                             args.prior, args.weight, args.top)
            summarise("tol %.2f  tight %5.1f" % (tol, tight), h, o, b, l)
    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

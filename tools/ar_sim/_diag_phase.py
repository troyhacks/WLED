#!/usr/bin/env python3
"""WHICH beat phase is the right one?  The strobe depends on it.

The strobe's phase accuracy budget is one render frame - 17-24 ms at the
default 42 fps.  So a 100 ms convention difference is not academic; it is
four frames of visible error.

There are two conventions in play:

  * a detector that fires on the frame where the band's energy RISES
    (this is what the AudioReactive ODF does), and
  * librosa's, whose onset envelope peaks 6 frames LATE on this material.

Those disagree by 100 ms, so "agreeing with librosa's beats" is not the
same as "agreeing with the kick".  This settles it against the audio
itself: the kick onsets are found in the raw signal by a threshold on
the 40-160 Hz band's short-term energy, with no reference to any
detector's convention, and then a beat grid is laid at each candidate
BPM and the offset that best lands on those onsets is reported.

Whichever detector's beats land on the kicks is the right one.

    python tools/ar_sim/_diag_phase.py
"""

import argparse
import os
import sys

import numpy as np
import scipy.signal as ss

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _oracle_librosa                             # noqa: E402
import _tempo_acf as T                             # noqa: E402
import _beat as B                                  # noqa: E402


def kick_onsets(x, sr, hop, thresh_rel=0.35, min_gap_s=0.20):
    """Onset times from the RAW band-limited energy rise.

    A frame whose short-term energy jumps by more than `thresh_rel` of
    the running maximum, at least `min_gap_s` after the last one.  No
    reference to any beat tracker - this is the sound itself.
    """
    sos = ss.butter(4, [40.0 / (sr / 2), 160.0 / (sr / 2)], btype="band",
                    output="sos")
    y = ss.sosfiltfilt(sos, x)
    n = (len(y) // hop) * hop
    p = (y[:n].reshape(-1, hop) ** 2).mean(axis=1)
    rise = np.maximum(np.diff(p, prepend=p[0]), 0.0)
    if rise.max() <= 0:
        return np.zeros(0)
    r = rise / rise.max()
    gap = max(1, int(min_gap_s * sr / hop))
    out = []
    last = -10 ** 9
    for i in np.where(r > thresh_rel)[0]:
        if i - last < gap:
            continue
        out.append(i * hop / sr)
        last = i
    return np.asarray(out)


def best_offset(bpm, grid_times, target_times, span=0.25, step=0.005):
    """The offset that best slides `grid_times` onto `target_times`."""
    if not len(grid_times) or not len(target_times):
        return None, 0.0
    best = (None, -1.0)
    for off in np.arange(-span, span + 1e-9, step):
        g = grid_times + off
        d = np.abs(target_times[:, None] - g[None, :]).min(axis=1)
        hit = float((d <= 0.070).mean())
        if hit > best[1]:
            best = (float(off), hit)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keys", default=None)
    ap.add_argument("--design", default="hp40_lp160x2")
    ap.add_argument("--whiten", type=float, default=0.0)
    ap.add_argument("--show", type=int, default=12)
    args = ap.parse_args()

    keys = _cache.keys()
    if args.keys:
        keys = [k for k in keys if args.keys in k]

    mine_off = []
    lib_off = []
    mine_hit = []
    lib_hit = []
    rows = []
    for k in keys:
        x = _cache.load(k)
        o = _oracle_librosa.load(k)
        if x is None or o is None or not o.get("beats"):
            continue
        onsets = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        if len(onsets) < 8:
            continue
        odf = T.odf_cached(k, x, args.design, args.whiten)
        mx = float(np.max(odf))
        odfn = odf / mx if mx > 0 else odf
        bpm, conf, det = B.estimate_dp(odf, win=1125)
        if bpm is None:
            continue
        peaks, strengths = B.peak_pick(odfn)
        first = max(0, len(odfn) - 1125)
        keep = peaks >= first
        peaks = peaks[keep]
        strengths = strengths[keep]
        path, _m = B.dp_beat_track(peaks, strengths, 60.0 * T.RATE / bpm)
        mine = peaks[path] / T.RATE

        # the oracle's beats, put on the same grid (a beat list IS a grid)
        lib = np.asarray(o["beats"], dtype=float)
        # the oracle's tempo, from its own beat spacing
        ibi = np.diff(lib)
        lib_bpm = 60.0 / float(np.median(ibi)) if len(ibi) else o["bpm"]

        om, hm = best_offset(bpm, mine, onsets)
        ol, hl = best_offset(lib_bpm, lib, onsets)
        if om is None or ol is None:
            continue
        mine_off.append(om)
        lib_off.append(ol)
        mine_hit.append(hm)
        lib_hit.append(hl)
        rows.append((k, bpm, lib_bpm, om, hm, ol, hl))

    if not rows:
        print("no rows")
        return
    mo = np.array(mine_off)
    lo = np.array(lib_off)
    print("%d windows" % len(rows))
    print()
    print("the offset that best slides a beat grid onto the RAW kick onsets")
    print("(a grid that lands on the kicks has offset ~0):")
    print("  our beats    offset median %+6.1f ms   hit rate median %5.3f"
          % (1000.0 * float(np.median(mo)), float(np.median(mine_hit))))
    print("  librosa's    offset median %+6.1f ms   hit rate median %5.3f"
          % (1000.0 * float(np.median(lo)), float(np.median(lib_hit))))
    print()
    print("our beats land on the kicks:      %5.1f%% of the windows"
          % (100.0 * float((np.abs(mo) < 0.040).mean())))
    print("librosa's beats land on the kicks: %5.1f%% of the windows"
          % (100.0 * float((np.abs(lo) < 0.040).mean())))
    print()
    print("worst cases (our offset):")
    rows.sort(key=lambda r: -abs(r[3]))
    for k, bpm, lib_bpm, om, hm, ol, hl in rows[:args.show]:
        print("  %-42s ours %+7.1f ms (hit %4.2f)   librosa %+7.1f ms (hit %4.2f)"
              % (k[:42], 1000 * om, hm, 1000 * ol, hl))


if __name__ == "__main__":
    sys.exit(main())

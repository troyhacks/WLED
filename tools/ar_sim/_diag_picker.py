#!/usr/bin/env python3
"""Which difference in the peak picker costs the accuracy?

The firmware's picker finds about 20% MORE peaks than the offline one, and
the extra peaks are enough to pull the comb's rate off by 0.25 BPM - a
fifth of a percent, which is 23 ms of drift over a 12 s window.

The two pickers differ in exactly two ways:

  the max      the offline normalises by the maximum of the WHOLE buffer
                (40 s in the simulation); the firmware has no whole buffer,
                so it uses a running maximum with a decaying tail
  the mean      the offline's local mean is SYMMETRIC - it averages
                [j-33, j+33], which needs 33 frames of the future; the
                firmware's is TRAILING, [j-33, j]

Each is switched on and off here, with the peak count and the resulting
comb rate and hit rate, so the expensive one can be identified.

    python tools/ar_sim/_diag_picker.py --limit 8
"""

import argparse
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _beat as B                                  # noqa: E402
import _comb as C                                  # noqa: E402
from _device_odf import device_odf, DEVICE_RATE    # noqa: E402
import _fw_sim as F                                # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402
from _eval_tempo import hit_with_offset, grid_at   # noqa: E402


def picker(odf, max_mode="run", max_tau=5.0, mean_mode="trail",
           whole_max=None):
    """The peak picker, with the two differences switchable.

    max_mode   "run"    a running maximum with a decaying tail
              "whole"  the maximum of the whole buffer
    mean_mode  "trail"  the mean over [j-33, j]
              "sym"    the mean over [j-33, j+33]
    """
    decay = math.exp(-1.0 / (max_tau * F.RATE))
    hist = []
    peaks = []
    prev_peak_frame = -1000
    peak_strength = 0.0
    odf_max = 0.0
    n = len(odf)
    for i in range(n):
        v = float(odf[i])
        if max_mode == "whole":
            odf_max = whole_max
        elif v > odf_max:
            odf_max = v
        else:
            odf_max *= decay
        hist.append(v)
        if len(hist) > F.HIST_LEN:
            hist.pop(0)
        j = i - F.PEAK_POST
        if j >= F.PEAK_PRE and len(hist) >= F.HIST_LEN:
            base = len(hist) - F.PEAK_POST - 1
            oj = hist[base]
            seg = hist[base - F.PEAK_PRE: base + F.PEAK_POST + 1]
            if mean_mode == "sym":
                a = max(0, base - F.PEAK_MEAN_HALF)
                b = min(len(hist), base + F.PEAK_MEAN_HALF + 1)
                mj = float(np.mean(hist[a:b]))
            else:
                a = max(0, base - F.PEAK_MEAN_HALF)
                mj = float(np.mean(hist[a:base + 1]))
            on = oj / odf_max if odf_max > 0 else 0.0
            if on >= F.PEAK_FLOOR and oj >= max(seg) \
                    and oj >= (1.0 + F.PEAK_DELTA) * mj:
                if j - prev_peak_frame < F.PEAK_MIN_GAP:
                    if on > peak_strength:
                        prev_peak_frame = j
                        peak_strength = on
                        peaks[-1] = (j, on)
                else:
                    prev_peak_frame = j
                    peak_strength = on
                    peaks.append((j, on))
    return peaks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=8)
    args = ap.parse_args()

    variants = [
        ("run5  trail", dict(max_mode="run", max_tau=5.0, mean_mode="trail")),
        ("run30 trail", dict(max_mode="run", max_tau=30.0, mean_mode="trail")),
        ("whole trail", dict(max_mode="whole", mean_mode="trail")),
        ("run5  sym", dict(max_mode="run", max_tau=5.0, mean_mode="sym")),
        ("whole sym", dict(max_mode="whole", mean_mode="sym")),
    ]
    tot = {v[0]: [] for v in variants}
    cnt = {v[0]: [] for v in variants}

    for k in _cache.keys()[:args.limit]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = device_odf(x)
        if len(odf) < 1200:
            continue
        win = int(12.0 * DEVICE_RATE)
        first = max(0, len(odf) - win)
        t_hi = len(odf) / DEVICE_RATE
        t_lo = first / DEVICE_RATE
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        wo = ons[(ons >= t_lo - 0.02) & (ons <= t_hi)]
        if len(wo) < 4:
            continue
        wmax = float(np.max(odf))
        line = "  %-38s" % k[:38]
        for name, kw in variants:
            pk = picker(odf, whole_max=wmax, **kw)
            pk = [(a, b) for a, b in pk if a >= first]
            if len(pk) < 8:
                continue
            fr = np.array([a for a, _b in pk], dtype=float)
            sg = np.array([b for _a, b in pk], dtype=float)
            bpm, score, phase = F.comb_full(fr, sg)
            g = grid_at(bpm, phase, first, len(odf))
            f, _o = hit_with_offset(bpm, g / DEVICE_RATE, wo)
            tot[name].append(f)
            cnt[name].append(len(pk))
            line += "  %s %6.2f %4.2f/%3d" % (name.split()[0], bpm, f, len(pk))
        print(line)

    print()
    for name, _kw in variants:
        a = np.array(tot[name])
        c = np.array(cnt[name])
        if not len(a):
            continue
        print("  %-14s hit median %5.3f   >=0.5: %5.1f%%   peaks median %5.1f"
              % (name, float(np.median(a)), 100.0 * float((a >= 0.5).mean()),
                 float(np.median(c))))


if __name__ == "__main__":
    sys.exit(main())

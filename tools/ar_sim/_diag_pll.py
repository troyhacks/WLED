#!/usr/bin/env python3
"""The comb's search or the PLL's grid - which one costs the accuracy?

The bisect has narrowed it to two things:

    off/new                 0.811   (offline peaks, offline comb, static grid)
    fw/new                  0.793   (firmware peaks, firmware comb, PLL grid)
    offPEAKS + fwCOMB       0.811   (offline peaks, firmware's comb, static grid)
    fwPEAKS + fwCOMB        0.811   (firmware peaks, firmware's comb, static grid)

so the PEAKS are innocent and the COMB's 64-bin full sweep is innocent.
What is left is the coarse-to-fine search (which finds the rate) and the PLL
grid (which places the beats).

This script runs the firmware's frame loop four ways, with the comb and the
grid swapped:

    full comb + static grid
    full comb + PLL grid
    c2f  comb + static grid
    c2f  comb + PLL grid      <- the firmware

and then sweeps the PLL's re-anchor gain, because a gain of 0.35 applied
140 times in a 12 s window is a random walk if the comb's phase is noisy.

    python tools/ar_sim/_diag_pll.py --limit 25
"""

import argparse
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_sim as F                                # noqa: E402
from _device_odf2 import device_odf_fw              # noqa: E402
from _diag_fw import firmware_peaks                # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402
from _eval_tempo import hit_with_offset, grid_at   # noqa: E402

WIN = int(12.0 * F.RATE)


def frame_loop(odf, comb="c2f", gamma=0.35, slew=0.005, alpha=0.0):
    """The firmware's frame loop, with the comb and the grid's gain free.

    Returns (beats, peaks, bpm, score).
    """
    state = F.TempoState()
    hist = []
    peaks = []
    prev_peak_frame = -1000
    peak_strength = 0.0
    odf_max = 0.0
    countdown = 0
    beats = []

    for i in range(len(odf)):
        v = float(odf[i])
        if v > odf_max:
            odf_max = v
        else:
            odf_max *= F.MAX_DECAY
        hist.append(v)
        if len(hist) > F.HIST_LEN:
            hist.pop(0)

        if state.locked:
            while i >= state.next_beat:
                beats.append(state.next_beat)
                state.beat_index += 1
                state.next_beat += state.period

        j = i - F.PEAK_POST
        if j >= F.PEAK_PRE and len(hist) >= F.HIST_LEN:
            base = len(hist) - F.PEAK_POST - 1
            oj = hist[base]
            seg = hist[base - F.PEAK_PRE: base + F.PEAK_POST + 1]
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
                if state.locked and alpha > 0.0:
                    st = state.period
                    e = j - (state.next_beat - st)
                    e -= st * round(e / st)
                    if abs(e) <= F.PLL_GATE * F.RATE:
                        state.next_beat += alpha * e

        countdown -= 1
        if countdown <= 0:
            countdown = F.SEARCH_EVERY
            keep = [p for p in peaks if p[0] >= i - F.COMB_WINDOW_S * F.RATE]
            if len(keep) >= 8:
                fr = np.array([p[0] for p in keep], dtype=float)
                sg = np.array([p[1] for p in keep], dtype=float)
                if comb == "c2f":
                    bpm, score, phase = F.comb_search(fr, sg)
                else:
                    bpm, score, phase = F.comb_full(fr, sg)
                if score >= F.MIN_SCORE:
                    state.score = score
                    adopt(state, bpm, phase, i, gamma, slew)

    return beats, peaks, state.bpm, state.score


def adopt(state, bpm, phase, now, gamma, slew):
    P = 60.0 * F.RATE / bpm
    if not state.locked:
        state.period = P
        state.next_beat = phase + round((now - phase) / P) * P
        state.locked = True
    else:
        lim = slew * state.period
        state.period += max(-lim, min(lim, P - state.period))
        k = round((state.next_beat - phase) / state.period)
        anchor = phase + k * state.period
        state.next_beat += gamma * (anchor - state.next_beat)
    state.bpm = 60.0 * F.RATE / state.period


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=25)
    args = ap.parse_args()

    variants = [
        ("full + static", None, None, 0.0),
        ("full + pll g.35", "full", 0.35, 0.0),
        ("c2f  + static", None, None, 0.0),
        ("c2f  + pll g.35", "c2f", 0.35, 0.0),
        ("c2f  + pll g.10", "c2f", 0.10, 0.0),
        ("c2f  + pll g.03", "c2f", 0.03, 0.0),
        ("c2f  + pll g.00", "c2f", 0.00, 0.0),
        ("c2f  + pll g.35 a.2", "c2f", 0.35, 0.2),
    ]
    tot = {v[0]: [] for v in variants}

    for k in _cache.keys()[:args.limit]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = device_odf_fw(x)
        if len(odf) < 1200:
            continue
        first = max(0, len(odf) - WIN)
        t_hi = len(odf) / F.RATE
        t_lo = first / F.RATE
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        wo = ons[(ons >= t_lo - 0.02) & (ons <= t_hi)]
        if len(wo) < 4:
            continue

        # the static grid needs the firmware's peaks and one comb call
        fp = firmware_peaks(odf)
        fp = [(a, b) for a, b in fp if a >= first]
        if len(fp) < 8:
            continue
        fr = np.array([p[0] for p in fp], dtype=float)
        sg = np.array([p[1] for p in fp], dtype=float)

        line = "  %-40s" % k[:40]
        for name, comb, gamma, alpha in variants:
            if comb is None:
                if name.startswith("full"):
                    bpm, score, phase = F.comb_full(fr, sg)
                else:
                    bpm, score, phase = F.comb_search(fr, sg)
                g = grid_at(bpm, phase, first, len(odf))
                f, _o = hit_with_offset(bpm, g / F.RATE, wo)
            else:
                beats, _pk, _b, _s = frame_loop(odf, comb=comb, gamma=gamma,
                                                alpha=alpha)
                b12 = np.array([b for b in beats if b >= first], dtype=float)
                if not len(b12):
                    f = 0.0
                else:
                    f, _o = hit_with_offset(bpm if False else 120.0,
                                            b12 / F.RATE, wo)
            tot[name].append(f)
            line += " %5.2f" % f
        print(line)

    print()
    for name, _c, _g, _a in variants:
        a = np.array(tot[name])
        if not len(a):
            continue
        print("  %-22s median %5.3f   >=0.5: %5.1f%%   >=0.8: %5.1f%%"
              % (name, float(np.median(a)), 100.0 * float((a >= 0.5).mean()),
                 100.0 * float((a >= 0.8).mean())))


if __name__ == "__main__":
    sys.exit(main())

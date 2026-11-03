#!/usr/bin/env python3
"""The firmware's constants, settled by measurement.

What is known now:

    full sweep (341 candidates at 0.25 BPM) + static grid   0.811  100.0%  60.0%
    coarse-to-fine       + static grid                      0.811  100.0%  60.0%
    coarse-to-fine       + the PLL grid                     0.793  100.0%  50.0%
    full sweep           + the PLL grid                     0.811   95.0%  55.0%

So the coarse-to-fine search is innocent on its own and the PLL is innocent
on its own, but TOGETHER they lose: the coarse-to-fine's rate flickers
between neighbouring candidates, and the PLL integrates every flicker into
the grid's position.  That is the "skipped beats" failure mode - a grid
that drifts inside the beat instead of staying on it.

THE SWEEP IS THE EXPENSIVE PART, so it is done ONCE and the variants are
REPLAYED over its results.  The comb's answer at a given frame does not
depend on the PLL's gain, on the slew, or on how often the search runs - the
search sees the same peaks whenever it happens.  So:

  pass 1   run the frame loop, and at every eighth frame RECORD the comb's
           (bpm, phase, score).  That is the expensive part.
  pass 2   replay those records through the PLL at each cadence, gain and
           slew, and score the grid.  That is cheap.

    python tools/ar_sim/_diag_const.py --limit 25
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_sim as F                                # noqa: E402
from _device_odf2 import device_odf_fw              # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402
from _eval_tempo import hit_with_offset            # noqa: E402

RATE = F.RATE
WIN = int(12.0 * RATE)
TAIL_S = 22.0          # how much of the track the frame loop runs over


def record_searches(odf, bins):
    """Pass 1: the frame loop, recording the comb's answer every eighth frame.

    Returns a list of (frame, bpm, phase, score).
    """
    F.NBINS = bins
    hist = []
    peaks = []
    prev_peak_frame = -1000
    peak_strength = 0.0
    odf_max = 0.0
    records = []
    n = len(odf)
    for i in range(n):
        v = float(odf[i])
        if v > odf_max:
            odf_max = v
        else:
            odf_max *= F.MAX_DECAY
        hist.append(v)
        if len(hist) > F.HIST_LEN:
            hist.pop(0)

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

        if i % 8 == 0:
            keep = [p for p in peaks if p[0] >= i - F.COMB_WINDOW_S * RATE]
            if len(keep) >= 8:
                fr = np.array([p[0] for p in keep], dtype=float)
                sg = np.array([p[1] for p in keep], dtype=float)
                bpm, score, phase = F.comb_full(fr, sg)
                records.append((i, bpm, phase, score))
    F.NBINS = 64
    return records


def replay(records, every, gamma, slew, first, nframes):
    """Pass 2: the PLL, fed the recorded searches."""
    state = F.TempoState()
    beats = []
    rec = 0
    for i in range(nframes):
        if state.locked:
            while i >= state.next_beat:
                beats.append(state.next_beat)
                state.beat_index += 1
                state.next_beat += state.period
        if i % every == 0 and rec < len(records):
            _f, bpm, phase, score = records[rec]
            rec += 1
            if score >= F.MIN_SCORE:
                adopt(state, bpm, phase, i, gamma, slew)
    return np.array([b for b in beats if b >= first], dtype=float)


def adopt(state, bpm, phase, now, gamma, slew):
    P = 60.0 * RATE / bpm
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
    state.bpm = 60.0 * RATE / state.period


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=25)
    args = ap.parse_args()

    variants = [
        ("bins 64  every  8  g.35 s.005", 64, 8, 0.35, 0.005),
        ("bins 256 every  8  g.35 s.005", 256, 8, 0.35, 0.005),
        ("bins 64  every 16  g.35 s.005", 64, 16, 0.35, 0.005),
        ("bins 64  every 32  g.35 s.005", 64, 32, 0.35, 0.005),
        ("bins 64  every 64  g.35 s.005", 64, 64, 0.35, 0.005),
        ("bins 64  every  8  g.05 s.005", 64, 8, 0.05, 0.005),
        ("bins 64  every  8  g1.0 s.005", 64, 8, 1.00, 0.005),
        ("bins 64  every  8  g.35 s.02 ", 64, 8, 0.35, 0.020),
        ("bins 64  every  8  g.35 s.10 ", 64, 8, 0.35, 0.100),
        ("bins 64  every 32 g.35 s.10 ", 64, 32, 0.35, 0.100),
    ]
    tot = {v[0]: [] for v in variants}

    for k in _cache.keys()[:args.limit]:
        x = _cache.load(k)
        if x is None:
            continue
        odf_full = device_odf_fw(x)
        if len(odf_full) < 1200:
            continue
        odf = odf_full[-int(TAIL_S * RATE):]
        nframes = len(odf)
        first = max(0, nframes - WIN)
        t_hi = nframes / RATE
        t_lo = first / RATE
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        wo = ons[(ons >= t_lo - 0.02) & (ons <= t_hi)]
        if len(wo) < 4:
            continue

        recs = {64: record_searches(odf, 64)}
        if any(v[1] == 256 for v in variants):
            recs[256] = record_searches(odf, 256)

        line = "  %-38s" % k[:38]
        for name, bins, every, gamma, slew in variants:
            b12 = replay(recs[bins], every, gamma, slew, first, nframes)
            if not len(b12):
                f = 0.0
            else:
                f, _o = hit_with_offset(120.0, b12 / RATE, wo)
            tot[name].append(f)
            line += " %5.2f" % f
        print(line)

    print()
    for name, _b, _e, _g, _s in variants:
        a = np.array(tot[name])
        if not len(a):
            continue
        print("  %-30s median %5.3f   >=0.5: %5.1f%%   >=0.8: %5.1f%%"
              % (name, float(np.median(a)), 100.0 * float((a >= 0.5).mean()),
                 100.0 * float((a >= 0.8).mean())))


if __name__ == "__main__":
    sys.exit(main())

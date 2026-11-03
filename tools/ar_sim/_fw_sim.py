#!/usr/bin/env python3
"""THE FIRMWARE ALGORITHM, SIMULATED.  This is what the C++ will do.

Everything before this file measured an ALGORITHM.  This one measures the
IMPLEMENTATION - the constraints the C++ actually lives under, which are
different in kind from the ones an offline experiment enjoys:

  * the ODF's running MAXIMUM replaces the offline "normalise by the max
    of the whole 40 s buffer", because the firmware has no whole buffer;
  * the peak picker's local-mean window is TRAILING, because the offline
    version's symmetric window needs 33 frames of the future;
  * the phase histogram has 64 bins, not 256, because the firmware pays
    for every bin on every candidate;
  * the rate search is COARSE-TO-FINE (1 BPM, then +/-1.2 BPM at 0.05),
    because 341 full candidates do not fit in a 10.67 ms frame;
  * the beat grid is not the comb's grid - it is a PLL that runs every
    frame, slews the period toward the estimate, nudges its phase toward
    the onsets, and keeps the grid CONTINUOUS across rate changes.

If the firmware shape scores like the offline shape, the port is a
translation.  If it does not, the port needs a different algorithm and
that is much cheaper to learn here than on the board.

    python tools/ar_sim/_fw_sim.py --limit 100
"""

import argparse
import math
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402
from _device_odf import device_odf, DEVICE_RATE    # noqa: E402

# --------------------------------------------------------------------------
# The firmware's constants.  These are the numbers the C++ will carry.
# --------------------------------------------------------------------------

RATE = DEVICE_RATE                 # 93.75 frames/second, exactly
BPM_LO, BPM_HI = 90.0, 175.0

NBINS = 64                         # the phase histogram's bins
TOL_S = 0.070                      # Ellis' F1 tolerance
TOL_FRAMES = TOL_S * RATE          # 6.5625 frames

PRIOR_BPM = 125.0                  # Troy's 128-135 working range
PRIOR_SIGMA = 0.35                 # ln units, ~ +/-27% at one sigma
PRIOR_WEIGHT = 0.6                 # validated: 97.3% >=0.5 on the corpus

PEAK_PRE = 3                       # frames
PEAK_POST = 3
PEAK_DELTA = 0.35                  # must beat the local mean by this
PEAK_MEAN_HALF = 33                # 0.35 s of local mean
PEAK_FLOOR = 0.06                  # fraction of the running max
PEAK_MIN_GAP = 5                   # frames
HIST_LEN = PEAK_MEAN_HALF + PEAK_POST + 1

MAX_TAU_S = 5.0                    # the running max's decay time constant
MAX_DECAY = math.exp(-1.0 / (MAX_TAU_S * RATE))

SEARCH_EVERY = 8                   # frames between rate searches (85 ms)
COMB_WINDOW_S = 12.0               # how much history the comb score looks at
COARSE_STEP = 1.0                  # BPM
FINE_STEP = 0.05                   # BPM
FINE_HALF = 1.2                    # BPM either side of a coarse winner
COARSE_KEEP = 3                    # how many coarse winners get refined

PLL_ALPHA = 0.20                   # the phase correction per onset
PLL_GATE = 0.040                   # only onsets this close correct the phase
PLL_SLEW = 0.005                   # the period may move this fraction per search
PLL_GAMMA = 0.35                   # how far the anchor is trusted per search

# These three are swept from the command line.
ALPHA_OVERRIDE = None
GATE_OVERRIDE = None
GAMMA_OVERRIDE = None

MIN_SCORE = 0.10                   # below this the comb is not believed


def prior_penalty(bpm):
    return PRIOR_WEIGHT * (math.log(bpm / PRIOR_BPM) / PRIOR_SIGMA) ** 2


def comb_score(frames, strengths, bpm, n_bins=NBINS):
    """The comb score at one candidate rate.  (score, phase_frames)."""
    total = float(np.sum(strengths))
    if total <= 0 or not len(frames):
        return 0.0, 0.0
    period = 60.0 * RATE / bpm
    u = np.mod(frames, period)
    bins = np.clip(np.floor(u / period * n_bins).astype(int), 0, n_bins - 1)
    h = np.zeros(n_bins)
    np.add.at(h, bins, strengths)
    w = max(1, int(round(TOL_FRAMES / period * n_bins)))
    h3 = np.concatenate((h, h, h))
    pre = np.concatenate(([0.0], np.cumsum(h3)))
    i = np.arange(n_bins)
    win = pre[n_bins + i + w + 1] - pre[n_bins + i - w]
    j = int(np.argmax(win))
    return float(win[j] / total), float(j / n_bins * period)


def comb_full(frames, strengths, step=0.25):
    """The firmware's comb, but a FULL sweep.  (bpm, score, phase_frames)."""
    best = None
    b = BPM_LO
    while b <= BPM_HI + 1e-9:
        s, ph = comb_score(frames, strengths, b)
        v = s - prior_penalty(b)
        if best is None or v > best[0]:
            best = (v, s, b, ph)
        b += step
    return best[2], best[1], best[3]


def comb_search(frames, strengths):
    """Coarse-to-fine.  (bpm, score, phase_frames)."""
    coarse = []
    b = BPM_LO
    while b <= BPM_HI + 1e-9:
        s, ph = comb_score(frames, strengths, b)
        coarse.append((s - prior_penalty(b), s, b, ph))
        b += COARSE_STEP
    coarse.sort(reverse=True)
    best = None
    for _v, _s, b, _ph in coarse[:COARSE_KEEP]:
        lo = max(BPM_LO, b - FINE_HALF)
        hi = min(BPM_HI, b + FINE_HALF)
        fb = lo
        while fb <= hi + 1e-9:
            s2, ph2 = comb_score(frames, strengths, fb)
            v = s2 - prior_penalty(fb)
            if best is None or v > best[0]:
                best = (v, s2, fb, ph2)
            fb += FINE_STEP
    return best[2], best[1], best[3]


class TempoState(object):
    """The firmware's tempo state.  One instance, like the C++'s."""

    def __init__(self):
        self.period = 0.0       # frames per beat
        self.next_beat = 0.0    # absolute frame time of the NEXT beat
        self.beat_index = 0
        self.locked = False
        self.score = 0.0
        self.bpm = 0.0
        self.searches = 0
        self.adopted = 0


def adopt(state, bpm, phase, now):
    """The PLL's rate update and re-anchor."""
    P = 60.0 * RATE / bpm
    if not state.locked:
        state.period = P
        state.next_beat = phase + round((now - phase) / P) * P
        state.locked = True
    else:
        lim = PLL_SLEW * state.period
        state.period += max(-lim, min(lim, P - state.period))
        k = round((state.next_beat - phase) / state.period)
        anchor = phase + k * state.period
        g = GAMMA_OVERRIDE if GAMMA_OVERRIDE is not None else PLL_GAMMA
        state.next_beat += g * (anchor - state.next_beat)
    state.bpm = 60.0 * RATE / state.period


def firmware_frame_loop(odf, state):
    """The firmware's frame loop, done the way the C++ will do it."""
    n = len(odf)
    hist = []            # the recent ODF values
    beats = []
    peaks = []           # (frame, strength)
    prev_peak_frame = -1000
    peak_strength = 0.0
    odf_max = 0.0
    countdown = 0

    for i in range(n):
        v = float(odf[i])
        if v > odf_max:
            odf_max = v
        else:
            odf_max *= MAX_DECAY
        hist.append(v)
        if len(hist) > HIST_LEN:
            hist.pop(0)

        # ---- the beat grid advances every frame -------------------------
        if state.locked:
            while i >= state.next_beat:
                beats.append(state.next_beat)
                state.beat_index += 1
                state.next_beat += state.period

        # ---- the candidate peak, PEAK_POST frames back ------------------
        j = i - PEAK_POST
        if j >= PEAK_PRE and len(hist) >= HIST_LEN:
            base = len(hist) - PEAK_POST - 1        # index of j inside hist
            oj = hist[base]
            seg = hist[base - PEAK_PRE: base + PEAK_POST + 1]
            a = max(0, base - PEAK_MEAN_HALF)
            mj = float(np.mean(hist[a:base + 1]))
            on = oj / odf_max if odf_max > 0 else 0.0
            if on >= PEAK_FLOOR and oj >= max(seg) \
                    and oj >= (1.0 + PEAK_DELTA) * mj:
                if j - prev_peak_frame < PEAK_MIN_GAP:
                    if on > peak_strength:
                        prev_peak_frame = j
                        peak_strength = on
                        peaks[-1] = (j, on)
                else:
                    prev_peak_frame = j
                    peak_strength = on
                    peaks.append((j, on))
                # the onset correction, once the peak is accepted
                if state.locked and ALPHA_OVERRIDE is not None:
                    st = state.period
                    e = j - (state.next_beat - st)
                    e -= st * round(e / st)
                    gate = GATE_OVERRIDE if GATE_OVERRIDE is not None else PLL_GATE
                    if abs(e) <= gate * RATE:
                        state.next_beat += ALPHA_OVERRIDE * e

        # ---- the rate search --------------------------------------------
        countdown -= 1
        if countdown <= 0:
            countdown = SEARCH_EVERY
            state.searches += 1
            keep = [p for p in peaks if p[0] >= i - COMB_WINDOW_S * RATE]
            if len(keep) >= 8:
                fr = np.array([p[0] for p in keep], dtype=float)
                sg = np.array([p[1] for p in keep], dtype=float)
                bpm, score, phase = comb_search(fr, sg)
                if score >= MIN_SCORE:
                    state.score = score
                    adopt(state, bpm, phase, i)
                    state.adopted += 1

    return beats, peaks


def hit_with_offset(beats, onsets, span=0.15, step=0.005):
    if not len(beats) or not len(onsets):
        return 0.0, 0.0
    best = (0.0, 0.0)
    for off in np.arange(-span, span + 1e-9, step):
        d = np.abs(onsets[:, None] - (beats + off)[None, :]).min(axis=1)
        f = float((d <= 0.070).mean())
        if f > best[0]:
            best = (f, float(off))
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=100)
    ap.add_argument("--show", type=int, default=12)
    ap.add_argument("--alpha", type=float, default=None,
                    help="the per-onset phase correction; 0 disables it")
    ap.add_argument("--gate", type=float, default=None)
    ap.add_argument("--gamma", type=float, default=None)
    ap.add_argument("--window", type=float, default=None,
                    help="the comb score's history window, seconds")
    args = ap.parse_args()

    globals()["ALPHA_OVERRIDE"] = args.alpha
    globals()["GATE_OVERRIDE"] = args.gate
    globals()["GAMMA_OVERRIDE"] = args.gamma
    globals()["COMB_WINDOW_S"] = args.window if args.window else COMB_WINDOW_S

    keys = _cache.keys()[:args.limit]
    fs = []
    fs12 = []
    rows = []
    t0 = time.time()
    for k in keys:
        x = _cache.load(k)
        if x is None:
            continue
        odf = device_odf(x)
        if len(odf) < 1200:
            continue
        state = TempoState()
        beats_f, peaks = firmware_frame_loop(odf, state)
        if not len(beats_f):
            continue
        t_hi = len(odf) / RATE
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        wo = ons[(ons >= -0.02) & (ons <= t_hi)]
        if len(wo) < 4:
            continue
        f, off = hit_with_offset(np.array(beats_f) / RATE, wo)
        # the last 12 s only - the span the offline metric used
        w12 = wo[(ons >= t_hi - 12.0) & (ons <= t_hi)]
        b12 = np.array([b for b in beats_f if b / RATE >= t_hi - 12.0]) / RATE
        f12, _o12 = hit_with_offset(b12, w12) if len(b12) and len(w12) else (0.0, 0.0)
        fs.append(f)
        fs12.append(f12)
        rows.append((k, state.bpm, f, f12, len(peaks), state.adopted))

    a = np.array(fs)
    a12 = np.array(fs12)
    print("%d windows   %.0f s   comb window %.1f s"
          % (len(rows), time.time() - t0, COMB_WINDOW_S))
    print()
    print("THE FIRMWARE SHAPE: running max, trailing mean, 64 bins,")
    print("coarse-to-fine search, and a PLL that runs every frame")
    print()
    print("  whole span   median %5.3f   >=0.5: %5.1f%%   >=0.8: %5.1f%%"
          % (float(np.median(a)), 100.0 * float((a >= 0.5).mean()),
             100.0 * float((a >= 0.8).mean())))
    print("  last 12 s    median %5.3f   >=0.5: %5.1f%%   >=0.8: %5.1f%%"
          % (float(np.median(a12)), 100.0 * float((a12 >= 0.5).mean()),
             100.0 * float((a12 >= 0.8).mean())))
    print()
    print("the offline shape, for comparison: median 0.870   "
          ">=0.5:  98.7%   >=0.8: 68.0%")
    print()
    rows.sort(key=lambda r: r[2])
    for k, bpm, f, f12, npk, ad in rows[:args.show]:
        print("  %-42s bpm %7.2f  hit %5.3f  last12 %5.3f  %d peaks"
              % (k[:42], bpm, f, f12, npk))


if __name__ == "__main__":
    sys.exit(main())

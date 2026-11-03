#!/usr/bin/env python3
"""RULE A (firmware) vs RULE B (best raw candidate outside the window).

A: the veto's far side is the whole-band VALUE winner (score - prior).
B: the veto's far side is the best RAW-score candidate outside the narrow
   window; the value argmax stays as the search's own winner.

Metrics: hit rate on the onsets, wander, jumps, re-lock time after a cut.
"""

import argparse
import math
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402

TOL_BPM = 1.0
HOLD_FRAMES = 281


class RuleB(P.Tracker):
    """The veto's far side is the best RAW candidate outside the window.

    mode:
      always  - compare every whole-band search; adopt the far raw candidate
      trigger - compare only when the value winner is outside the window;
                adopt the value winner when the veto grants (grid still goes
                to the search's own winner)
      trigger_raw - same trigger, adopt the far raw candidate
    """

    mode = "always"

    def sweep_start(self):
        cutoff = self.frame_index - P.COMB_FRAMES
        keep = [(f, s) for (f, s) in self.peak_frame if f >= cutoff]
        if self.top_n and len(keep) > self.top_n:
            keep = sorted(keep, key=lambda t: -t[1])[:self.top_n]
        self.sweep_frames = np.array([f for f, _ in keep], dtype=float)
        self.sweep_str = np.array([s for _, s in keep], dtype=float)
        self.sweep_index = 0
        self.sweep_now = float(self.frame_index)
        self.last_sweep_frame = self.frame_index
        narrow = (self.locked and not self.need_full
                  and self.narrow_bpm > 0.0)
        self.sweep_reason = self.need_full_reason
        self.need_full = False
        self.need_full_reason = None
        self.sweep_was_narrow = narrow
        if not narrow:
            self.last_full_frame = self.frame_index
        self.sweep_best_value = -1e9
        self.sweep_best_score = 0.0
        self.sweep_best_bpm = 0.0
        self.sweep_best_phase = 0.0
        self.sweep_narrow_first = -1
        self.sweep_narrow_count = 0
        self.sweep_narrow_score = 0.0
        self.sweep_narrow_bpm = 0.0
        self.sweep_narrow_phase = 0.0
        self.sweep_active = len(keep) >= P.MIN_PEAKS
        if narrow:
            lo = max(P.BPM_LO, self.bpm - self.narrow_bpm)
            hi = min(P.BPM_HI, self.bpm + self.narrow_bpm)
            self.sweep_clipped_lo = (lo <= P.BPM_LO)
            self.sweep_clipped_hi = (hi >= P.BPM_HI)
            f, c = P.narrow_window(self.bpm, self.narrow_bpm)
            bpms = P.BPM_LO + np.arange(f, f + c) * self.cand_step
        else:
            self.sweep_clipped_lo = False
            self.sweep_clipped_hi = False
            bpms = P.BPM_LO + np.arange(self.cand_count) * self.cand_step
        self.sweep_bpms = bpms
        self.cand_total += len(bpms)
        if narrow:
            self.search_narrow += 1
        else:
            self.search_full += 1
        if not self.sweep_active:
            return
        if narrow:
            pen = np.array([P.PRIOR_WEIGHT * (math.log(b / P.PRIOR_BPM) / P.PRIOR_SIGMA) ** 2
                            for b in bpms])
        else:
            pen = self.prior
        scores, phases = self.comb_scores_all(self.sweep_frames,
                                              self.sweep_str, bpms)
        values = scores - pen
        j = int(np.argmax(values))
        if (not narrow) and self.locked and self.narrow_bpm > 0.0 \
                and (self.veto_on_collapse or self.sweep_reason != 'collapse'):
            f, c = P.narrow_window(self.bpm, self.narrow_bpm)
            self.sweep_narrow_first = f
            self.sweep_narrow_count = c
            idx = np.arange(f, f + c)
            jn = int(idx[np.argmax(scores[idx])])
            self.sweep_narrow_score = float(scores[jn])
            self.sweep_narrow_bpm = float(bpms[jn])
            self.sweep_narrow_phase = float(phases[jn])
            alive = float(scores[jn]) >= P.MIN_SCORE
            # RULE B: the far side is the best RAW candidate OUTSIDE the window
            mask = np.ones(len(scores), dtype=bool)
            mask[f:f + c] = False
            outside = np.where(mask)[0]
            jv = j
            if len(outside):
                jb = int(outside[np.argmax(scores[outside])])
                trigger = True
                if self.mode != "always":
                    trigger = not (f <= jv <= f + c - 1)
                if trigger:
                    self.net_far += 1
                    if alive and float(scores[jb]) < float(scores[jn]) * (1.0 + self.net_margin):
                        self.net_vetoes += 1
                        j = jn
                    else:
                        j = jb if self.mode != "trigger" else jv
        self.sweep_best_value = float(values[j])
        self.sweep_best_score = float(scores[j])
        self.sweep_best_bpm = float(bpms[j])
        self.sweep_best_phase = float(phases[j])
        self.sweep_best_index = j


def make(cls, gain=0.10, margin=0.20):
    t = cls()
    t.narrow_bpm = 2.0
    t.search_every = 4
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = 32
    t.net_margin = margin
    t.pll_gain = gain
    return t


def wander(t):
    log = np.array(t.bpm_log)
    half = log[len(log) // 2:]
    half = half[half > 0]
    if len(half) < 100:
        return 0.0, 0, 0.0
    jumps = int(np.sum(np.abs(np.diff(half)) > 0.5))
    biggest = float(np.max(np.abs(np.diff(half)))) if len(half) > 1 else 0.0
    return float(np.std(half)), jumps, biggest


def reference_bpm(odf):
    t = P.Tracker()
    t.narrow_bpm = 0.0
    t.run(odf)
    return t.bpm if t.locked and t.bpm > 0 else None


def relock_time(t, cut, ref):
    log = t.bpm_log
    n = len(log)
    first = None
    for f in range(cut, n):
        if t.locked and abs(log[f] - ref) <= TOL_BPM:
            first = f
            break
    if first is None:
        return None
    for f in range(first, min(n, first + HOLD_FRAMES)):
        if not t.locked or abs(log[f] - ref) > 2.0 * TOL_BPM:
            return None
    return (first - cut) / P.RATE


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=18)
    ap.add_argument("--pairs", type=int, default=8)
    args = ap.parse_args()
    t0 = time.time()
    items = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, 40.0, 160.0)
        if len(odf) < 1200:
            continue
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        if len(ons) < 4:
            continue
        items.append((k, odf, ons))
    refs = [reference_bpm(it[1]) for it in items]
    pairs = []
    for i in range(len(items)):
        if refs[i] is None:
            continue
        for j in range(i + 1, len(items)):
            if refs[j] is None or abs(refs[j] - refs[i]) < 6.0:
                continue
            pairs.append((items[i], items[j], refs[j]))
            break
        if len(pairs) >= args.pairs:
            break
    print("%d windows %d pairs  %.0f s" % (len(items), len(pairs), time.time() - t0),
          flush=True)
    print("%-6s %8s %8s %8s %8s | %10s %8s %8s"
          % ("rule", "hit", "wander", "jumps", "biggest", "relock s", "worst", "never"))
    for name, cls, mode in (("A", P.Tracker, None),
                            ("B_trig", RuleB, "trigger")):
        hits, wand, jumps, bigs = [], [], [], []
        for (k, odf, ons) in items:
            t = make(cls)
            if mode:
                t.mode = mode
            t.run(odf)
            if not t.beats:
                hits.append(0.0)
                continue
            f, _off = P.hit_with_offset(np.array(t.beats) / P.RATE, ons)
            hits.append(f)
            w, j, b = wander(t)
            wand.append(w)
            jumps.append(j)
            bigs.append(b)
        times, never = [], 0
        for (a, b, ref) in pairs:
            t = make(cls)
            if mode:
                t.mode = mode
            cut = len(a[1])
            t.run(np.concatenate((a[1], b[1])))
            r = relock_time(t, cut, ref)
            if r is None:
                never += 1
                times.append(999.0)
            else:
                times.append(r)
        arr = np.array(times)
        print("%-6s %8.3f %8.3f %8.1f %8.3f | %10.2f %8.2f %8d"
              % (name, float(np.median(hits)), float(np.mean(wand)),
                 float(np.mean(jumps)), float(np.mean(bigs)),
                 float(np.median(arr)), float(arr.max()), never), flush=True)
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""A FAITHFUL EMULATION OF THE CURRENT ar_tempo.h, diffed against _fw_port.py.

The C++ now:
  * computes the narrow window by snapping the CENTRE to the candidate grid
    (arTempoNarrowWindow), the same as the port's narrow_window();
  * picks the narrow winner by RAW SCORE, not by value;
  * spreads the whole-band sweep over eight frames and applies the veto at
    the END of the sweep;
  * refuses to veto when the local rate's score is below AR_TEMPO_MIN_SCORE
    ("a dead local rate cannot keep the grid");
  * runs the PLL proportionally, with AR_TEMPO_PLL_GAIN.

The port has the first four except the MIN_SCORE guard, so this diff finds
exactly that one difference - and says how often it matters.

    python tools/ar_sim/_cppparity2.py --windows 10
"""

import argparse
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402

NARROW = 2.0
EVERY = 4
NET = 32
MARGIN = 0.20
GAIN = 0.10


class CppTracker(P.Tracker):
    """ar_tempo.h, faithfully: the veto at the end of the spread sweep."""

    def __init__(self):
        super().__init__()
        self.sweep_index = 0
        self.sweep_first = 0
        self.sweep_n = 0
        self.sweep_per_frame = P.CAND_PER_FRAME
        self.sweep_total = 0.0
        self.sweep_best_idx = 0
        self.sweep_has_narrow = False
        self.sweep_narrow_first = 0
        self.sweep_narrow_count = 0
        self.sweep_narrow_value = -1e9
        self.sweep_narrow_score = -1e9
        self.sweep_narrow_bpm = 0.0
        self.sweep_narrow_phase = 0.0
        self.sweep_narrow_idx = 0
        self.sweep_frames = None
        self.sweep_str = None
        self.guard_skips = 0     # the veto was skipped: the local rate is dead
        self.guard_events = []

    # ---- arTempoSweepStart() ----------------------------------------
    def sweep_start(self):
        cutoff = self.frame_index - P.COMB_FRAMES
        keep = [(f, s) for (f, s) in self.peak_frame if f >= cutoff]
        self.sweep_frames = np.array([f for f, _ in keep], dtype=float)
        self.sweep_str = np.array([s for _, s in keep], dtype=float)
        self.sweep_total = float(np.sum(self.sweep_str)) if len(keep) else 0.0
        self.sweep_index = 0
        self.sweep_now = float(self.frame_index)
        self.last_sweep_frame = self.frame_index

        narrow = (self.locked and not self.need_full
                  and self.narrow_bpm > 0.0 and self.bpm > 0.0)
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
        self.sweep_best_idx = 0
        self.sweep_narrow_value = -1e9
        self.sweep_narrow_score = -1e9
        self.sweep_narrow_bpm = 0.0
        self.sweep_narrow_phase = 0.0
        self.sweep_narrow_idx = 0
        self.sweep_has_narrow = False
        self.sweep_narrow_first = 0
        self.sweep_narrow_count = 0

        if narrow:
            f, c = P.narrow_window(self.bpm, self.narrow_bpm)
            self.sweep_first = f
            self.sweep_n = c
        else:
            self.sweep_first = 0
            self.sweep_n = self.cand_count
            if self.locked and self.bpm > 0.0:
                f, c = P.narrow_window(self.bpm, self.narrow_bpm)
                self.sweep_narrow_first = f
                self.sweep_narrow_count = c
                self.sweep_has_narrow = True

        self.sweep_per_frame = min(self.sweep_n, P.CAND_PER_FRAME)
        self.sweep_active = len(keep) >= P.MIN_PEAKS
        if narrow:
            self.search_narrow += 1
        else:
            self.search_full += 1

    # ---- arTempoSweepStep() ------------------------------------------
    def sweep_step(self):
        if not self.sweep_active:
            return
        end = min(self.sweep_n, self.sweep_index + self.sweep_per_frame)
        if end > self.sweep_index:
            idxs = np.arange(self.sweep_first + self.sweep_index,
                              self.sweep_first + end)
            bpms = P.BPM_LO + idxs * self.cand_step
            scores, phases = self.comb_scores_all(self.sweep_frames,
                                                   self.sweep_str, bpms)
            pen = np.array([
                P.PRIOR_WEIGHT * (math.log(b / P.PRIOR_BPM) / P.PRIOR_SIGMA) ** 2
                for b in bpms])
            values = scores - pen
            for k in range(len(idxs)):
                idx = int(idxs[k])
                v = float(values[k])
                if v > self.sweep_best_value:
                    self.sweep_best_value = v
                    self.sweep_best_score = float(scores[k])
                    self.sweep_best_bpm = float(bpms[k])
                    self.sweep_best_phase = float(phases[k])
                    self.sweep_best_idx = idx
                # THE NARROW WINNER: by RAW SCORE, not by value.
                if (self.sweep_has_narrow
                        and self.sweep_narrow_first <= idx
                        < self.sweep_narrow_first + self.sweep_narrow_count):
                    if float(scores[k]) > self.sweep_narrow_score:
                        self.sweep_narrow_score = float(scores[k])
                        self.sweep_narrow_value = v
                        self.sweep_narrow_bpm = float(bpms[k])
                        self.sweep_narrow_phase = float(phases[k])
                        self.sweep_narrow_idx = idx
            self.cand_total += len(idxs)
            self.sweep_index = end
        if self.sweep_index < self.sweep_n:
            return

        # THE VETO, with the C++'s guard on the local rate's score.
        if (self.sweep_has_narrow and self.sweep_narrow_score > -1e8
                and self.sweep_narrow_score >= P.MIN_SCORE):
            firstN = self.sweep_narrow_first
            lastN = firstN + self.sweep_narrow_count - 1
            inside = firstN <= self.sweep_best_idx <= lastN
            if not inside:
                self.net_far += 1
                if (self.sweep_best_score
                        < self.sweep_narrow_score * (1.0 + self.net_margin)):
                    self.net_vetoes += 1
                    self.sweep_best_value = self.sweep_narrow_value
                    self.sweep_best_score = self.sweep_narrow_score
                    self.sweep_best_bpm = self.sweep_narrow_bpm
                    self.sweep_best_phase = self.sweep_narrow_phase
                    self.sweep_best_idx = self.sweep_narrow_idx
        elif self.sweep_has_narrow and self.sweep_narrow_score > -1e8:
            # the C++'s guard: a dead local rate does not get to veto
            self.guard_skips += 1
            self.guard_events.append((self.frame_index, self.sweep_reason,
                                      self.sweep_narrow_score,
                                      self.sweep_best_score,
                                      self.sweep_best_bpm))

        self.score = self.sweep_best_score
        if self.sweep_best_score >= P.MIN_SCORE:
            self.adopt(self.sweep_best_bpm, self.sweep_best_phase, self.sweep_now)
        elif self.sweep_was_narrow:
            self.need_full = True
            self.need_full_reason = 'collapse'
        self.sweep_active = False


def make(kind):
    t = P.Tracker() if kind == 'port' else CppTracker()
    t.narrow_bpm = NARROW
    t.search_every = EVERY
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = NET
    t.net_margin = MARGIN
    t.veto_on_collapse = True
    t.pll_gain = GAIN
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=10)
    args = ap.parse_args()

    nwin = 0
    tot_diff = 0
    tot_guard = 0
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, 80.0, 320.0)
        if len(odf) < 1200:
            continue
        nwin += 1
        a = make('port')
        a.run(odf)
        b = make('cpp')
        b.run(odf)
        la = np.array(a.bpm_log)
        lb = np.array(b.bpm_log)
        n = min(len(la), len(lb))
        d = np.abs(la[:n] - lb[:n])
        bad = np.where(d > 1e-3)[0]
        tot_diff += len(bad)
        tot_guard += b.guard_skips
        print("%-40s frames %5d  veto port %3d cpp %3d  far port %3d cpp %3d"
              "  guard-skips %3d  differing frames %3d"
              % (k[:40], n, a.net_vetoes, b.net_vetoes, a.net_far, b.net_far,
                 b.guard_skips, len(bad)))
        for e in b.guard_events[:4]:
            print("      guard: frame %6d reason %-8s narrow %.4f  far %.4f "
                  "at %.2f" % (e[0], str(e[1]), e[2], e[3], e[4]))
        if len(bad):
            for i in bad[:8]:
                print("        frame %6d  port %8.3f  cpp %8.3f"
                      % (i, la[i], lb[i]))
    print()
    print("%d windows, %d differing frames, %d guard skips" %
          (nwin, tot_diff, tot_guard))


if __name__ == "__main__":
    sys.exit(main())

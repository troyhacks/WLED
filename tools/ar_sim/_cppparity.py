#!/usr/bin/env python3
"""A FAITHFUL EMULATION OF ar_tempo.h's SEARCH, to diff against _fw_port.py.

The C++ spreads the whole-band sweep over eight frames and applies the veto
at the END of the sweep; the port scores everything at sweep start and
applies the veto there.  This emulation does what the C++ does, step for
step, and the two bpm_logs are compared frame by frame.

    python tools/ar_sim/_cppparity.py --windows 6
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
        self.sweep_narrow_score = 0.0
        self.sweep_narrow_bpm = 0.0
        self.sweep_narrow_phase = 0.0
        self.sweep_frames = None
        self.sweep_str = None
        self.veto_log = []

    # ---- the window, as the C++ computes it --------------------------
    def _window(self, bpm):
        lo = max(P.BPM_LO, bpm - self.narrow_bpm)
        hi = min(P.BPM_HI, bpm + self.narrow_bpm)
        f = int(math.ceil((lo - P.BPM_LO) / self.cand_step))
        c = int(math.floor((hi - P.BPM_LO) / self.cand_step)) - f + 1
        if f < 0:
            f = 0
        if c < 1:
            c = 1
        if f + c > self.cand_count:
            c = self.cand_count - f
        return f, c

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
        self.sweep_narrow_score = 0.0
        self.sweep_narrow_bpm = 0.0
        self.sweep_narrow_phase = 0.0
        self.sweep_has_narrow = False
        self.sweep_narrow_first = 0
        self.sweep_narrow_count = 0

        if narrow:
            f, c = self._window(self.bpm)
            self.sweep_first = f
            self.sweep_n = c
        else:
            self.sweep_first = 0
            self.sweep_n = self.cand_count
            if self.locked and self.bpm > 0.0:
                f, c = self._window(self.bpm)
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
                if (self.sweep_has_narrow
                        and self.sweep_narrow_first <= idx
                        < self.sweep_narrow_first + self.sweep_narrow_count):
                    if v > self.sweep_narrow_value:
                        self.sweep_narrow_value = v
                        self.sweep_narrow_score = float(scores[k])
                        self.sweep_narrow_bpm = float(bpms[k])
                        self.sweep_narrow_phase = float(phases[k])
            self.cand_total += len(idxs)
            self.sweep_index = end
        if self.sweep_index < self.sweep_n:
            return

        # THE VETO
        if self.sweep_has_narrow and self.sweep_narrow_value > -1e8:
            firstN = self.sweep_narrow_first
            lastN = firstN + self.sweep_narrow_count - 1
            inside = firstN <= self.sweep_best_idx <= lastN
            if not inside:
                self.net_far += 1
                if (self.sweep_best_score
                        < self.sweep_narrow_score * (1.0 + self.net_margin)):
                    self.net_vetoes += 1
                    self.veto_log.append((self.frame_index, self.sweep_best_bpm,
                                          self.sweep_best_score,
                                          self.sweep_narrow_bpm,
                                          self.sweep_narrow_score))
                    self.sweep_best_value = self.sweep_narrow_value
                    self.sweep_best_score = self.sweep_narrow_score
                    self.sweep_best_bpm = self.sweep_narrow_bpm
                    self.sweep_best_phase = self.sweep_narrow_phase
                    self.sweep_best_idx = firstN

        self.score = self.sweep_best_score
        if self.sweep_best_score >= P.MIN_SCORE:
            self.adopt(self.sweep_best_bpm, self.sweep_best_phase, self.sweep_now)
        elif self.sweep_was_narrow:
            self.need_full = True
            self.need_full_reason = 'collapse'
        self.sweep_active = False


def make_port(margin):
    t = P.Tracker()
    t.narrow_bpm = NARROW
    t.search_every = EVERY
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = NET
    t.net_margin = margin
    t.veto_on_collapse = True
    return t


def make_cpp(margin):
    t = CppTracker()
    t.narrow_bpm = NARROW
    t.search_every = EVERY
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = NET
    t.net_margin = margin
    t.veto_on_collapse = True
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=6)
    ap.add_argument("--margin", type=float, default=0.20)
    args = ap.parse_args()

    nwin = 0
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, 80.0, 320.0)
        if len(odf) < 1200:
            continue
        nwin += 1
        a = make_port(args.margin)
        a.run(odf)
        b = make_cpp(args.margin)
        b.run(odf)
        la = np.array(a.bpm_log)
        lb = np.array(b.bpm_log)
        n = min(len(la), len(lb))
        d = np.abs(la[:n] - lb[:n])
        bad = np.where(d > 1e-3)[0]
        print("%-44s frames %5d  vetoes port %3d cpp %3d  far port %3d cpp %3d"
              % (k[:44], n, a.net_vetoes, b.net_vetoes, a.net_far, b.net_far))
        print("      max |bpm_port - bpm_cpp| = %.6f   differing frames: %d"
              % (float(d.max()) if n else 0.0, len(bad)))
        if len(bad):
            for i in bad[:12]:
                print("        frame %6d  port %8.3f  cpp %8.3f"
                      % (i, la[i], lb[i]))
    print()
    print("%d windows" % nwin)


if __name__ == "__main__":
    sys.exit(main())

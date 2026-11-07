#!/usr/bin/env python3
"""THE FLICKER: why the published rate wobbles, and what holds it.

Troy's report:

  "I still see flickers between, say, 128 and 127 and back to 128,
   which causes the beat grid to fall out of sync.  We aren't 'trusting'
   the established BPM and clock strongly enough during 'confused' moments
   like breakdowns and transitions."

  "The BPM doing minor fluctuations is a good sign of being confused."

  "in DJ Set mode, we expect a fairly steady tempo and the beat grid to
   shift very mildly - if we see a bunch of grid modifications, that's
   confusion - keep the steady state and adjust once we have a better lock."

  "No DJ is going to go 128, 129, 128 over a few seconds."

So this is the INSTRUMENT: it runs the tracker and prints, for every
narrow search, what the comb said, what the grid said, and what the grid
did.  Two numbers matter:

  the RATE the comb proposed      - the measurement, "where are we now?"
  the published BPM                - the grid, "where should we be?"

The gap between them IS the confusion.  A tracker that publishes the
measurement is a tracker that flickers; a tracker that publishes a HELD
grid and only moves it on good evidence is what Troy is asking for.

    python tools/ar_sim/_bg_flicker.py
"""

import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _fw_bg as B                                     # noqa: E402
import _fw_port as P                                   # noqa: E402
import _cache                                          # noqa: E402


class Flicker(B.BgTracker):
    """The tracker, with the narrow search's every answer written down."""

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.answers = []      # (frame, rate, score, local, bpm_before)

    def narrow_result(self, rate, phase, score, local):
        before = self.bpm
        super().narrow_result(rate, phase, score, local)
        self.answers.append((self.frame_index, rate, score, local, before))

    def sweep_step(self):
        """The parent's, but the narrow search's own bookkeeping kept."""
        if self.sweep_active and self.sweep_frames is not None \
                and len(self.sweep_frames) > 0:
            gidx = self.sweep_gidx
            bpms, scores, phases, pick = self._score_the(self.sweep_frames,
                                                         self.sweep_str, gidx)
            rate, peak = self.rate_from(scores, bpms,
                                        np.where((gidx % 4) == 2,
                                                 self.whole_bonus, 0.0),
                                        use_prior=not self.locked)
            self.plateau = (float(rate), int(peak),
                            float(scores[peak]),
                            self.last_plateau[0], self.last_plateau[1])
        else:
            self.plateau = None
        super().sweep_step()


def wobble(log, t0, t1=None):
    a = int(t0 * P.RATE)
    b = len(log) if t1 is None else min(len(log), int(t1 * P.RATE))
    tail = log[a:b]
    if len(tail) == 0:
        return 0.0
    return float(np.max(tail) - np.min(tail))


def flips(t, tol=0.05):
    """How many times the comb's answer changed SIDE, and how many times the
    published rate moved by more than `tol`."""
    side = 0
    moves = 0
    last_side = 0
    last_bpm = None
    for (_f, rate, _s, _l, before) in t.answers:
        if last_bpm is None:
            last_bpm = before
        if last_side != 0 and rate - before != 0.0:
            if (rate - before > 0) != (last_side > 0):
                side += 1
        if abs(rate - before) > tol:
            moves += 1
        last_side = rate - before
        last_bpm = before
    return side, moves, len(t.answers)


def case_steady():
    """A steady 128 with noise.  The rate must NOT move at all."""
    print("== the steady 128: what does the comb say, frame by frame")
    odf = B.beat_odf([(128.0, 0.0, 40.0, 1.0)], dur=40.0, noise=0.05)
    t = Flicker()
    t.run(odf)
    log = np.array(t.bpm_log)
    side, moves, n = flips(t)
    rates = np.array([a[1] for a in t.answers])
    print("   %d narrow searches   the comb's rate: min %.3f  max %.3f  "
          "std %.4f" % (n, rates.min(), rates.max(), rates.std()))
    print("   side flips %d   rate moves >0.05 BPM %d" % (side, moves))
    print("   the published BPM: min %.3f  max %.3f  wobble %.3f   "
          "nudges %d  change %d"
          % (log.min(), log.max(), wobble(log, 15), t.nudges, t.changes))
    # the first few answers, so the shape is visible
    for (f, rate, s, l, before) in t.answers[:8]:
        print("      f%5d  comb %.4f  score %.3f  local %.3f  grid %.4f"
              % (f, rate, s, l, before))
    return t


def case_confused():
    """The confusion: a breakdown and a transition inside a steady 128."""
    print("== the confusion: 128, five seconds of nothing, 128, a weak "
          "transition, 128")
    odf = B.beat_odf([(128.0, 0.0, 15.0, 1.0),
                      (128.0, 20.0, 10.0, 1.0),
                      (128.0, 32.0, 10.0, 0.35)], dur=45.0)
    t = Flicker()
    t.run(odf)
    log = np.array(t.bpm_log)
    side, moves, n = flips(t)
    print("   %d narrow searches   side flips %d   rate moves >0.05 BPM %d"
          % (n, side, moves))
    print("   the published BPM: min %.3f  max %.3f   wobble %.3f   "
          "nudges %d  change %d  refused %d"
          % (log.min(), log.max(), wobble(log, 15), t.nudges, t.changes,
             t.bg_refused))
    return t


def case_corpus():
    """The real music: how much does the published rate move, and how often?

    This is the number Troy sees: the flicker on real tracks.
    """
    print("== the corpus: the real music, the published rate's wobble")
    t0 = time.time()
    keys = _cache.keys()
    wob = []
    for k in keys[:40]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.odf_of(x)
        if len(odf) < 1200:
            continue
        t = Flicker()
        t.run(odf)
        if not t.locked or t.bpm <= 0:
            continue
        log = np.array(t.bpm_log)
        # the last 10 s: the steady state, after the lock
        wob.append(wobble(log, max(0.0, len(log) / P.RATE - 10.0)))
    w = np.array(wob)
    print("   %d windows   %.0f s" % (len(w), time.time() - t0), flush=True)
    if len(w):
        print("   the published rate's wobble over the last 10 s: "
              "median %.3f   mean %.3f   90th %.3f   max %.3f BPM"
              % (np.median(w), w.mean(), np.percentile(w, 90), w.max()))
        print("   windows that moved more than 0.5 BPM: %d of %d"
              % (int((w > 0.5).sum()), len(w)))


def main():
    case_steady()
    case_confused()
    case_corpus()
    return 0


if __name__ == "__main__":
    sys.exit(main())

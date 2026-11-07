#!/usr/bin/env python3
"""TWO ESTIMATORS, ONE GRID - the offline twin of the new tempo tracker.

Troy's design, as he put it:

  "I feel we need two sets of data here - the 'steady state' with a steady
   BPM and a self-aligning beat grid, and then another that's checking the
   live data and making its own guesses.  If we've been steady at, say, 128...
   let's keep that going unless the 'background' estimators converge and lock
   RELIABLY on another tempo and grid.  If we lose the sync from the live
   music, keep the existing bpm and grid."

What the twin is for: the tracker in _fw_port.py is a line-by-line twin of
usermods/audioreactive/ar_tempo.h.  This file is the twin of the REWRITE.

THREE RULES carry the design.

1. THE RATE IS THE PLATEAU'S CENTRE, NOT ITS ARGMAX.

   The comb's score SATURATES: on a clean 128 BPM track every candidate from
   126.5 to 129.0 scores exactly 1.000, because the kernel is wide enough to
   hold them all.  The argmax of a flat run is its FIRST member - so the old
   tracker published the rate at the EDGE of the band of rates it could not
   tell apart, and it wandered by half a BPM at random.  Measured, the argmax
   of a clean 128 track reads 127.50, 128.00, 128.50 at different moments;
   the plateau's CENTRE reads 128.00, 128.12, 128.00.  So the rate is the
   centre of the run of candidates the comb cannot separate.

2. THE RATE DOES NOT MOVE FOR A MIxture.

   A short excursion - 128, then three seconds of 127, then 128 - leaves the
   window holding TWO grids, and the comb's best fit for a mixture is a
   compromise rate that is NEITHER (measured: 125.75).  So the rate only
   moves when the new rate is either a NUDGE (close to the rate the grid is
   at) or the music really changed (the rate the grid is at has died).

3. THE BACKGROUND HAS TO EARN IT.

   The whole band is searched separately, on its own clock.  It nominates a
   rate and a grid, and it may only move the grid when it has CONVERGED -
   the same rate and the same grid over several searches in a row - and when
   its evidence is clearly better than the steady rate's on the SAME window.
   The old safety net, which handed the grid to a far candidate on a 5 %
   margin once the local rate had been declared dead, is GONE: that was the
   128 -> 160 breakout.

    python tools/ar_sim/_fw_bg.py --case all
"""

import argparse
import math
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_port as P                               # noqa: E402
from _fw_port import rnd                          # noqa: E402
from _fw_band import firmware_odf                  # noqa: E402


# --------------------------------------------------------------------------
# The knobs.  Every one of them is a constant in the firmware.
# --------------------------------------------------------------------------

BG_EVERY = 32                 # frames between background searches (~341 ms)
BG_CONVERGE = 6               # consecutive agreeing searches before it counts
BG_AGREE_BPM = 0.60           # the agreement, in BPM
BG_PHASE_TOL = 0.50           # the agreement, in beats
BG_MARGIN = 0.70              # the fight, on the SAME window
BG_MIN_PEAKS = 8              # fewer onsets than this and it says nothing

NEAR_BPM = 1.50               # a NUDGE: this close needs no margin
DEAD_ACCEPT = 0.50            # when the local rate is dead, a candidate must clear this
PLATEAU_TOL = 1e-4            # the relative width of the plateau's flat top

# THE CONFIRMATION.
#
# The comb's answer is a MEASUREMENT and the grid is a CLOCK.  A measurement
# is allowed to move the clock only when it is CONSISTENT, and consistency
# here means one thing: the answer has not REVERSED.
#
# A breakdown, a transition, a breakdown again - the answer goes 127.5,
# 128.0, 128.25, 127.75, 128.0.  It goes back the way it came.  That is
# not a tempo, it is a tracker that is confused, and a confused tracker
# must not be allowed to move the clock.  It is exactly Troy's:
#
#   "No DJ is going to go 128, 129, 128 over a few seconds."
#
# A DJ's slow tempo change, on the other hand, keeps saying the same thing
# in the same direction: 128.0, 128.25, 128.5, 128.75.  It never comes
# back.  So it, and only it, is allowed to move the clock.
#
# So the rule is a RUN: the answer has to keep going one way - or keep
# still - for PERSIST searches before the clock may move.  A reversal
# starts the run again from there.
#
#   MOVE     below this the answer has not really moved.  The lattice's
#            step is a quarter of a BPM, so this sees every real step.
#   PERSIST  how many searches the run has to last.  The narrow search runs
#            every four frames, so 116 searches is five seconds.
#
# Troy:
#
#   "keep the steady state and adjust once we have a better lock."
MOVE = 0.10
PERSIST = 116
# PLAYLIST wants less than a third of it: a track boundary is not something
# to be waited out, and the memory there is already half as long.
PERSIST_LIST = 39


# The two modes.  Each is a set of the constants above, and each one is a
# choice the firmware makes from the mode setting - so they belong HERE,
# next to the constants they change, and not in a script that sets them
# from outside.
#
#   DJ SET     One continuous performance.  The window holds one piece of
#              music, so the long memory is honest, and the background has
#              to CONVERGE (6 agreeing searches) and beat the steady rate
#              by a clear margin (0.70) before it may take the grid.
#
#   PLAYLIST   Anything can shuffle in.  A long window then holds TWO
#              tracks, so the memory is halved - the old track is forgotten
#              sooner and the new one is found sooner - and the handover
#              is quicker: fewer agreements (3) and a smaller margin (0.40).
MODES = {
    "dj set":    dict(window=P.COMB_FRAMES,     converge=6, margin=0.70,
                     persist=PERSIST),
    "playlist":  dict(window=P.COMB_FRAMES // 2, converge=3, margin=0.40,
                     persist=PERSIST_LIST),
}


class BgTracker(P.Tracker):
    """The new tracker: two estimators, one grid."""

    def __init__(self):
        super().__init__()
        # The firmware's default lattice: the half-BPM grid.
        self.quant = 1
        self.whole_bonus = 0.01
        self.bg_window = P.COMB_FRAMES
        self.bg_every = BG_EVERY
        self.bg_converge = BG_CONVERGE
        self.bg_agree_bpm = BG_AGREE_BPM
        self.bg_phase_tol = BG_PHASE_TOL
        self.bg_margin = BG_MARGIN
        self.bg_min_peaks = BG_MIN_PEAKS
        self.near_bpm = NEAR_BPM
        self.dead_accept = DEAD_ACCEPT
        self.persist = PERSIST
        self.mode = "dj set"

        # The background's state.
        self.bg_bpm = 0.0
        self.bg_phase = 0.0
        self.bg_score = 0.0
        self.bg_local_score = 0.0
        self.bg_agree = 0
        self.bg_converged = False
        self.bg_have = False
        self.bg_last_frame = -1000
        self.bg_searches = 0
        self.bg_refused = 0

        # Diagnostics.
        self.nudges = 0
        self.changes = 0
        self.held_frames = 0
        self.events = []
        self.last_plateau = (0, 0, 0, 0.0)

        # The confirmation: the measurement's consistency state.
        self.conf_base = 0.0
        self.conf_prev = 0.0
        self.conf_count = 0
        self.conf_dir = 0

    # ---- the mode -------------------------------------------------------

    def set_mode(self, mode):
        """The mode: 'dj set' or 'playlist'.

        The firmware reads the mode every frame and picks the constants
        from it, so the twin does the same - one call, and the three
        numbers that change are changed together.
        """
        cfg = MODES[mode]
        self.mode = mode
        self.bg_window = cfg["window"]
        self.bg_converge = cfg["converge"]
        self.bg_margin = cfg["margin"]
        self.persist = cfg["persist"]

    # ---- the plateau ----------------------------------------------------

    def _prior_penalty(self, bpm):
        return self.prior_weight * (math.log(bpm / P.PRIOR_BPM)
                                     / P.PRIOR_SIGMA) ** 2

    def rate_from(self, scores, bpms, wpen=None, use_prior=True):
        """The rate: the CENTRE of the best plateau.

        The comb cannot separate a band of nearby rates - the kernel is wide
        enough to hold them all - and the score is exactly flat across that
        band.  The argmax of a flat run is its FIRST member, which is where
        the old tracker published the rate and why it wandered by half a BPM
        at random.  So the rate is the CENTRE of the run.

        Which run?  The score has several peaks - the music's tempo, and
        the harmonics the comb also likes - and the PRIOR chooses between
        them, exactly as it chose between candidates before.  It cannot
        choose WITHIN a run: the prior's penalty varies by less than the
        plateau's tolerance across a run, so applying it to the run would
        cut the run short and drag the centre towards 125.  So: the prior
        picks the plateau, the geometry gives the rate inside it.

        `use_prior` is False for the NARROW search: the firmware's measured
        rule is that the prior votes in the whole-band search and not in the
        narrow one, because in a four-BPM window it is a tie-breaker that
        only ever pulls towards 125.

        Returns (rate, peak_index).
        """
        n = len(scores)
        best_val = -1e18
        best_rate = float(bpms[0]) if n else 0.0
        best_peak = 0
        for j in range(n):
            if j > 0 and scores[j] < scores[j - 1]:
                continue
            if j < n - 1 and scores[j] < scores[j + 1]:
                continue
            tol = max(1e-9, float(scores[j]) * PLATEAU_TOL)
            a = j
            while a > 0 and scores[a - 1] >= scores[j] - tol:
                a -= 1
            b = j
            while b < n - 1 and scores[b + 1] >= scores[j] - tol:
                b += 1
            centre = 0.5 * (float(bpms[a]) + float(bpms[b]))
            # The plateau's geometry, for the instrument.  The centre is the
            # rate; a and b are where the flat top ENDS, and they are what
            # moves when the score wobbles.
            self.last_plateau = (a, b, j, centre)
            val = float(scores[j])
            if use_prior:
                val -= self._prior_penalty(centre)
            if wpen is not None:
                val -= float(wpen[j])
            if val > best_val:
                best_val = val
                best_rate = centre
                best_peak = j
        return best_rate, best_peak

    # ---- the search -----------------------------------------------------

    def _score_the(self, frames, strs, gidx):
        bpms = P.BPM_LO + gidx * self.cand_step
        scores, phases = self.comb_scores_all(frames, strs, bpms)
        wpen = 0.0
        if self.quant and self.whole_bonus:
            wpen = np.where((gidx % 4) == 2, self.whole_bonus, 0.0)
        pick = scores - wpen
        return bpms, scores, phases, pick

    def sweep_step(self):
        """The narrow search: the steady state's own comb.

        The parent scores the candidates 44 at a frame; the twin takes them
        all at once.  The RESULT is the same set of scores.
        """
        if not self.sweep_active:
            return
        if self.sweep_frames is None or len(self.sweep_frames) == 0:
            self.sweep_active = False
            return
        gidx = self.sweep_gidx
        bpms, scores, phases, pick = self._score_the(self.sweep_frames,
                                                     self.sweep_str, gidx)
        wpen = 0.0
        if self.quant and self.whole_bonus:
            wpen = np.where((gidx % 4) == 2, self.whole_bonus, 0.0)
        rate, peak = self.rate_from(scores, bpms, wpen, use_prior=not self.locked)
        score = float(scores[peak])
        phase = float(phases[peak])
        self.score = score
        if score >= self.min_score:
            self.full_search_failed = False
            self.collapse_count = 0
            local = 0.0
            if self.locked:
                local = float(self.comb_score(self.sweep_frames,
                                              self.sweep_str, self.bpm)[0])
            self.narrow_result(rate, phase, score, local)
        else:
            # The narrow search cannot find the beat any more.  The grid
            # HOLDS: no adopt, so neither the rate nor the phase moves.
            self.collapse_count = min(255, self.collapse_count + 1)
        self.sweep_active = False

    # ---- the clock and the measurement ----------------------------------

    def adopt_rate(self, bpm, hard=False):
        """The RATE half of the clock: the period moves, the phase does not."""
        per = 60.0 * P.RATE / bpm
        if hard:
            self.period = per
        elif self.pll_gain > 0.0:
            self.period += self.pll_gain * (per - self.period)
        else:
            lim = P.PLL_SLEW * self.period
            d = per - self.period
            self.period += lim if d > lim else (-lim if d < 0 else d)
        self.bpm = 60.0 * P.RATE / self.period

    def adopt_phase(self, phase):
        """The PHASE half of the clock: the beat moves, the rate does not.

        The anchor is worked out with the CLOCK's own period, so a candidate
        rate that jitters does not make the anchor jitter with it.
        """
        k = rnd((self.next_beat - phase) / self.period)
        anchor = phase + k * self.period
        self.next_beat += P.PLL_GAMMA * (anchor - self.next_beat)

    def adopt(self, bpm, phase, now, hard=False):
        """The whole clock, for the moments when it moves as one."""
        if not self.locked:
            self.period = 60.0 * P.RATE / bpm
            self.next_beat = phase + rnd((now - phase) / self.period) * self.period
            self.locked = True
            self.beat_index = 0
            self.bpm = bpm
            return
        if hard:
            self.adopt_rate(bpm, hard=True)
            k = rnd((self.next_beat - phase) / self.period)
            self.next_beat = phase + k * self.period
        else:
            self.adopt_rate(bpm)
            self.adopt_phase(phase)

    def confirm(self, rate):
        """The RUN: how long has the answer kept going one way?

        A reversal - the answer going back the way it came - starts the run
        again from there.  A run that has lasted PERSIST searches has not
        reversed for that long, and that is the only thing the clock asks
        before it moves.

        Returns the length of the current run.
        """
        if self.conf_count == 0:
            self.conf_base = rate
            self.conf_prev = rate
            self.conf_count = 1
            self.conf_dir = 0
            return self.conf_count
        move = rate - self.conf_prev
        if abs(move) <= MOVE:
            # It has not really moved: the same answer again.  The run goes
            # on - a still answer is the most consistent answer there is.
            self.conf_count += 1
            self.conf_prev = rate
            return self.conf_count
        d = 1 if move > 0 else -1
        if self.conf_dir != 0 and d != self.conf_dir:
            # IT CAME BACK.  That is confusion, not a tempo.  The run starts
            # again from here.
            self.conf_base = rate
            self.conf_count = 1
            self.conf_dir = 0
        else:
            self.conf_dir = d
            self.conf_count += 1
        self.conf_prev = rate
        return self.conf_count

    def narrow_result(self, rate, phase, score, local):
        """The narrow search's winner - the steady state's own decision.

        THE CLOCK IS A CLOCK.  The comb's answer is a MEASUREMENT, and a
        measurement only moves the clock when it says the clock is wrong AND
        keeps saying it.  That is what stops the 128-127-128 flicker, and
        it is what lets a DJ's slow tempo change through, because a tempo
        change pushes the answer out of the dead band and leaves it there.

        THE PHASE ALWAYS FOLLOWS.  The comb's phase is a much better
        conditioned measurement than its rate - the phase histogram has a
        sharp peak even when the rate is ambiguous - so the grid tracks
        the music's beats while the rate stays where it is.
        """
        if not self.locked:
            self.adopt(rate, phase, self.sweep_now)
            self.conf_count = 0
            self.conf_dir = 0
            return
        # The RUN: the answer has to keep going one way - or keep still -
        # before it is believed.  It is watched whether it is near the clock
        # or far from it.
        run = self.confirm(rate)
        near = abs(rate - self.bpm) <= self.near_bpm
        if not near:
            # A DIFFERENT grid.
            if local < self.min_score and score >= self.dead_accept:
                # The music changed: re-anchor on the new grid rather than
                # walking through the rates where nothing aligns.
                self.adopt(rate, phase, self.sweep_now, hard=True)
                self.changes += 1
                return
            if run >= self.persist and score >= self.dead_accept:
                # The far answer has held its course, and it has evidence of
                # its own.  A MIXTURE - three seconds of 127 inside twelve
                # seconds of 128 - is not that: its best fit is a compromise
                # rate that nothing aligns with, and the score says so.
                self.adopt(rate, phase, self.sweep_now, hard=True)
                self.changes += 1
                return
            # Otherwise: HOLD.  The grid keeps its rate and its phase.
            return
        # The answer is in the clock's neighbourhood.  It may still be
        # telling the clock that its rate is wrong.
        if run >= self.persist:
            # CONFIRMED: the answer has held its course.  Move the clock's
            # rate, a step at a time.
            self.adopt_rate(rate)
            self.nudges += 1
        else:
            self.held_frames += 1
        self.adopt_phase(phase)

    # ---- the background estimator ---------------------------------------

    def bg_search(self):
        """The background's whole-band search over the RECENT onsets."""
        self.bg_searches += 1
        cutoff = self.frame_index - self.bg_window
        keep = [(f, s) for (f, s) in self.peak_frame if f >= cutoff]
        if len(keep) < self.bg_min_peaks:
            # Nothing to look at: a breakdown, or the music has not started.
            # It says NOTHING, and it does not accuse the steady state.
            self.bg_have = False
            self.bg_converged = False
            self.bg_agree = 0
            return
        frames = np.array([f for f, _ in keep], dtype=float)
        strs = np.array([s for _, s in keep], dtype=float)
        gidx = np.arange(self.cand_count)
        if self.quant:
            gidx = gidx[(gidx % 2) == 0]
        bpms, scores, phases, pick = self._score_the(frames, strs, gidx)
        wpen = 0.0
        if self.quant and self.whole_bonus:
            wpen = np.where((gidx % 4) == 2, self.whole_bonus, 0.0)
        rate, peak = self.rate_from(scores, bpms, wpen)
        score = float(scores[peak])
        phase = float(phases[peak])

        # The steady rate's OWN evidence, on the SAME window.  A fight where
        # one side has twelve seconds of memory and the other has five is
        # not a fight.
        self.bg_local_score = float(self.comb_score(frames, strs, self.bpm)[0])

        # The agreement: the same rate AND the same grid.
        if self.bg_have:
            d = abs(rate - self.bg_bpm)
            per = 60.0 * P.RATE / rate
            a = (self.bg_phase / per) % 1.0
            b = (phase / per) % 1.0
            dp = abs(a - b)
            dp = min(dp, 1.0 - dp)
            if d <= self.bg_agree_bpm and dp <= self.bg_phase_tol:
                self.bg_agree += 1
            else:
                self.bg_agree = 1
        else:
            self.bg_agree = 1

        self.bg_bpm = rate
        self.bg_phase = phase
        self.bg_score = score
        self.bg_have = True
        self.bg_converged = self.bg_agree >= self.bg_converge

    def bg_maybe(self):
        if not self.locked:
            return
        if self.frame_index - self.bg_last_frame < self.bg_every:
            return
        self.bg_last_frame = self.frame_index
        self.bg_search()
        self.bg_promote()

    def bg_promote(self):
        """The background's verdict: does the grid move?"""
        if not (self.bg_have and self.bg_converged):
            return False
        if self.bg_score < self.min_score:
            return False
        near = abs(self.bg_bpm - self.bpm) <= self.near_bpm
        if near:
            # A nudge: the background agrees with the grid about where the
            # tempo is, within the lattice's own resolution.
            #
            # The PHASE only.  The background's rate has earned its own
            # agreement - six searches that said the same rate AND the same
            # grid - but that agreement is only to within 0.6 BPM, and
            # pulling the clock 20 % toward a rate that wanders inside that
            # 0.6 BPM is the flicker this whole scheme exists to stop.  The
            # rate moves on the narrow path's CONFIRMATION, or on a change;
            # the background's long-window anchor is worth the phase.
            self.adopt_phase(self.bg_phase)
            return True
        if self.bg_local_score < self.min_score:
            # The rate the grid is at has died: the music changed, and the
            # background's answer has evidence of its own.
            if self.bg_score < self.dead_accept:
                self.bg_refused += 1
                return False
            self.adopt(self.bg_bpm, self.bg_phase, self.sweep_now, hard=True)
            self.changes += 1
            return True
        # A long way off, and the steady rate is still alive: the background
        # has to be CLEARLY better evidence.  This is what keeps a mixture's
        # compromise rate - a little better than the rate the grid has, not
        # a lot better - from taking the grid.
        if self.bg_score < self.bg_local_score * (1.0 + self.bg_margin):
            self.bg_refused += 1
            return False
        self.adopt(self.bg_bpm, self.bg_phase, self.sweep_now, hard=True)
        self.changes += 1
        return True

    # ---- the frame -------------------------------------------------------

    def frame(self, odf):
        self.frame_index += 1
        if odf > self.odf_max:
            self.odf_max = odf
        else:
            self.odf_max *= P.MAX_DECAY
        self.pick_peak(odf, self.frame_index)
        if self.locked:
            while self.frame_index >= self.next_beat:
                self.beats.append(self.next_beat)
                self.next_beat += self.period
                self.beat_index += 1
        self.bpm_log.append(self.bpm)
        self.score_log.append(self.score)
        self.bg_maybe()
        if not self.sweep_active:
            # While the grid is locked the ONLY whole-band search is the
            # background's.  The safety net - a whole-band search over the
            # comb's twelve seconds - is gone: it was the thing that handed
            # the grid to a far candidate on a 5 % margin.
            if (not self.locked
                    or self.frame_index - self.last_sweep_frame >= self.search_every):
                self.sweep_start()
        self.sweep_step()


# --------------------------------------------------------------------------
# The synthetic cases.
# --------------------------------------------------------------------------

def beat_odf(tracks, dur, noise=0.02, seed=1):
    """tracks: a list of (bpm, start, length, amplitude).

    `bpm` may be a callable of the time, for a ramp.  The amplitude is the
    spike's height, so a second track at 160 with amplitude 0.4 is "there is
    also a 160 in it, but quieter".

    Every section is PHASE-ALIGNED to the first section's grid: a section
    that starts at 25 s puts its first beat on the next beat of the grid
    the music was already on.  Without that the sections land at
    arbitrary phases and the comb sees two phase clusters that have nothing
    to do with each other - which is not what a breakdown or a tempo change
    looks like.
    """
    rng = np.random.default_rng(seed)
    n = int(round(dur * P.RATE))
    odf = np.zeros(n)
    p0 = 60.0 / (tracks[0][0](0.0) if callable(tracks[0][0]) else tracks[0][0])
    for (bpm, start, length, amp) in tracks:
        # the first beat on the reference grid, at or after `start`
        t = start + ((-start) % p0)
        end = start + length
        while t < end:
            f = int(round(t * P.RATE))
            if 0 <= f < n:
                odf[f] += amp
            b = bpm(t) if callable(bpm) else bpm
            t += 60.0 / b
    if noise:
        odf = odf + rng.random(n) * noise
    return odf


def odf_of(x):
    return firmware_odf(x, P.BAND_LO, P.BAND_HI)


def run(odf, **kw):
    t = BgTracker()
    for k, v in kw.items():
        setattr(t, k, v)
    t.run(odf)
    return t


def med(log, t0, t1=None):
    a = int(t0 * P.RATE)
    b = len(log) if t1 is None else min(len(log), int(t1 * P.RATE))
    tail = log[a:b]
    return float(np.median(tail)) if len(tail) else float("nan")


def case_breakdown():
    """A five-second breakdown in the middle of a track.

    The BPM must not move and the grid must not jump.
    """
    print("== the breakdown: 128 for 20 s, five seconds of nothing, 128 again")
    odf = beat_odf([(128.0, 0.0, 20.0, 1.0),
                    (128.0, 25.0, 20.0, 1.0)], dur=45.0)
    t = run(odf)
    log = np.array(t.bpm_log)
    dur = log[int(21 * P.RATE):int(24 * P.RATE)]
    print("   before %7.2f   during %7.2f   after %7.2f   "
          "max move during %5.2f   nudge %d  change %d  refused %d"
          % (med(log, 15, 19), med(log, 21, 24), med(log, 30),
             float(np.max(dur) - np.min(dur)) if len(dur) else 0.0,
             t.nudges, t.changes, t.bg_refused))
    return t


def case_ramp():
    """A slow ramp: 128 to 130 over thirty seconds."""
    print("== the ramp: 128 -> 130 over 30 s")
    ramp = lambda t: 128.0 + 2.0 * min(max((t - 5.0) / 30.0, 0.0), 1.0)
    odf = beat_odf([(ramp, 0.0, 40.0, 1.0)], dur=40.0)
    t = run(odf)
    log = np.array(t.bpm_log)
    q = [med(log, s, s + 3) for s in (5, 20, 34)]
    print("   at 5 s %7.2f   at 20 s %7.2f   at 34 s %7.2f   "
          "(want 128.0 / 129.0 / 129.9)   nudge %d  change %d"
          % (q[0], q[1], q[2], t.nudges, t.changes))
    return t


def case_breakout():
    """The music is 128, and there is also a 160 in it.

    The rate must stay at 128.
    """
    print("== the breakout: 128 with a 160 on top")
    for amp in (0.5, 0.8, 1.0):
        odf = beat_odf([(128.0, 0.0, 30.0, 1.0),
                        (160.0, 0.0, 30.0, amp)], dur=30.0)
        t = run(odf)
        log = np.array(t.bpm_log)
        print("   a 160 at %3.0f %%  ->  rate %7.2f   (want 128)   "
              "nudge %d  change %d  refused %d"
              % (amp * 100.0, med(log, 15), t.nudges, t.changes, t.bg_refused))
    return t


def case_change():
    """A real change: 128 for 20 s, then 130."""
    print("== the change: 128 for 20 s, then 130 for 20 s")
    odf = beat_odf([(128.0, 0.0, 20.0, 1.0),
                    (130.0, 20.0, 20.0, 1.0)], dur=40.0)
    t = run(odf)
    log = np.array(t.bpm_log)
    cut = int(20 * P.RATE)
    settle = None
    on = np.abs(log - 130.0) <= 0.6
    runlen = 0
    for i, v in enumerate(on[cut:], start=cut):
        runlen = runlen + 1 if v else 0
        if runlen >= 40:
            settle = (i - runlen + 1) / P.RATE
            break
    print("   at the cut %7.2f   5 s later %7.2f   at the end %7.2f   "
          "settled after %s   nudge %d  change %d"
          % (log[cut], log[min(len(log) - 1, cut + int(5 * P.RATE))], log[-1],
             ("%.2f s" % settle) if settle is not None else "never",
             t.nudges, t.changes))
    return t


def case_twitch():
    """128, then 127 for three seconds, then 128 again.

    The rate must not follow it.
    """
    print("== the twitch: 128, 127 for 3 s, 128")
    odf = beat_odf([(128.0, 0.0, 15.0, 1.0),
                    (127.0, 15.0, 3.0, 1.0),
                    (128.0, 18.0, 15.0, 1.0)], dur=33.0)
    t = run(odf)
    log = np.array(t.bpm_log)
    a = int(14 * P.RATE)
    b = int(19 * P.RATE)
    print("   before %7.2f   during %7.2f   after %7.2f   "
          "max move during %5.2f   nudge %d  change %d  refused %d"
          % (med(log, 12, 14), med(log, 14, 19), med(log, 22),
             float(np.max(log[a:b]) - np.min(log[a:b])),
             t.nudges, t.changes, t.bg_refused))
    return t


def case_corpus():
    """The real music: does the published rate still land on the truth?

    The truth is the rate the SAME tracker finds when it is given the window
    on its own - the reference the old scripts use.
    """
    print("== the corpus: 60 windows, the rate against the reference")
    t0 = time.time()
    items = []
    for k in _cache.keys()[:60]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = odf_of(x)
        if len(odf) < 1200:
            continue
        items.append((k, odf))
    print("   %d windows   %.0f s" % (len(items), time.time() - t0), flush=True)

    errs_new = []
    errs_old = []
    for (k, odf) in items:
        r = P.Tracker()
        r.narrow_bpm = 0.0
        r.run(odf)
        if not r.locked or r.bpm <= 0:
            continue
        ref = r.bpm
        n = run(odf)
        o = P.Tracker()
        o.quant = n.quant
        o.whole_bonus = n.whole_bonus
        o.run(odf)
        errs_new.append(abs(n.bpm - ref))
        errs_old.append(abs(o.bpm - ref))
    en = np.array(errs_new)
    eo = np.array(errs_old)
    print("   NEW  median %.3f BPM   mean %.3f   90th %.3f   n=%d"
          % (np.median(en), en.mean(), np.percentile(en, 90), len(en)))
    print("   OLD  median %.3f BPM   mean %.3f   90th %.3f   n=%d"
          % (np.median(eo), eo.mean(), np.percentile(eo, 90), len(eo)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--case", default="all")
    args = ap.parse_args()
    which = args.case
    if which in ("all", "breakdown"):
        case_breakdown()
    if which in ("all", "ramp"):
        case_ramp()
    if which in ("all", "breakout"):
        case_breakout()
    if which in ("all", "change"):
        case_change()
    if which in ("all", "twitch"):
        case_twitch()
    if which in ("all", "corpus"):
        case_corpus()
    return 0


if __name__ == "__main__":
    sys.exit(main())

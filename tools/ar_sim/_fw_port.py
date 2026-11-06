#!/usr/bin/env python3
"""THE C++ TRACKER, PORTED LINE BY LINE.  A check on ar_tempo.h.

Every constant below is the constant in usermods/audioreactive/ar_tempo.h, and
every step is the step the C++ takes.  The point is that the corpus numbers
that decided the design were measured on a PYTHON approximation; this runs the
actual algorithm's shape and compares the two bands that the approximation was
ambiguous about.

THE BAND QUESTION
-----------------
The offline work used _device_odf.py, which applies the 3 kHz-DESIGNED band
filters (hp40 x2 then lp160 x2) to the 6 kHz cache and then decimates by
two.  A filter designed for a corner f at a rate r lands at f*(run_rate/r)
on the stream it is actually run on, so those corners land at 80 Hz and
320 Hz.

The firmware does the opposite order: it decimates 48 kHz to 3 kHz FIRST and
then runs the 3 kHz-designed band (90-320 Hz) on the 3 kHz stream, so its
corners really are 90 Hz and 320 Hz.

So the two are close but not the same measurement - 90 Hz at the bottom
against the corpus's 80 Hz - and the corpus numbers that chose this
algorithm were measured on a band the board does not quite run.  This
script measures both.

    python tools/ar_sim/_fw_port.py --limit 300
"""

import argparse
import math
import os
import sys
import time

import numpy as np
from scipy.signal import lfilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402

# --------------------------------------------------------------------------
# The constants, from ar_tempo.h.
# --------------------------------------------------------------------------

DEC = 16                     # AR_TEMPO_DEC
RATE = 93.75                 # AR_TEMPO_RATE
FS = 3000.0                  # AR_TEMPO_FS
HOP = 32                     # AR_TEMPO_HOP
BAND_LO = 90.0               # AR_TEMPO_BAND_LO_HZ
BAND_HI = 320.0              # AR_TEMPO_BAND_HI_HZ
Q = 0.70710678               # AR_TEMPO_Q

PEAK_PRE = 3                 # AR_TEMPO_PEAK_PRE
PEAK_POST = 3                # AR_TEMPO_PEAK_POST
PEAK_DELTA = 0.35            # AR_TEMPO_PEAK_DELTA
PEAK_MEAN_HALF = 33          # AR_TEMPO_PEAK_MEAN_HALF
PEAK_FLOOR = 0.06            # AR_TEMPO_PEAK_FLOOR
PEAK_MIN_GAP = 5             # AR_TEMPO_PEAK_MIN_GAP
HIST_LEN = 37                # AR_TEMPO_HIST_LEN
HIST_RING = 64               # AR_TEMPO_HIST_RING

MAX_DECAY = math.exp(-1.0 / (5.0 * RATE))     # AR_TEMPO_MAX_DECAY

# The picker's sliding window, as an index array - so the window is ONE
# numpy gather instead of a 37-step Python loop.  This is the port's own
# speed-up; it does not change a single result.
_HIST_I = np.arange(HIST_LEN)

BPM_LO, BPM_HI = 90.0, 175.0                   # AR_TEMPO_BPM_LO/HI
CAND_STEP = 0.25             # AR_TEMPO_CAND_STEP
CAND_COUNT = 341             # AR_TEMPO_CAND_COUNT
COMB_BINS = 64               # AR_TEMPO_COMB_BINS
CAND_PER_FRAME = 44          # AR_TEMPO_CAND_PER_FRAME
COMB_FRAMES = 1125           # AR_TEMPO_COMB_FRAMES (12 s)

PRIOR_BPM = 125.0            # AR_TEMPO_PRIOR_BPM
PRIOR_SIGMA = 0.35           # AR_TEMPO_PRIOR_SIGMA
PRIOR_WEIGHT = 0.6           # AR_TEMPO_PRIOR_WEIGHT

MIN_SCORE = 0.35             # AR_TEMPO_MIN_SCORE
COLLAPSE_COUNT = 4           # AR_TEMPO_COLLAPSE_COUNT
MIN_PEAKS = 8                # AR_TEMPO_MIN_PEAKS
PLL_GAMMA = 0.35             # AR_TEMPO_PLL_GAMMA
PLL_SLEW = 0.005             # AR_TEMPO_PLL_SLEW
PEAK_RING = 256              # AR_TEMPO_PEAK_RING
TOL_MS = 70.0                # AR_TEMPO_TOL_DEFAULT_MS


# --------------------------------------------------------------------------
# The filters, as the C++ has them.
# --------------------------------------------------------------------------

def rbj_hpf(f0, fs, q):
    w0 = 2.0 * math.pi * f0 / fs
    cw = math.cos(w0)
    alpha = math.sin(w0) / (2.0 * q)
    a0 = 1.0 + alpha
    return [(1.0 + cw) * 0.5 / a0, -(1.0 + cw) / a0, (1.0 + cw) * 0.5 / a0,
            -2.0 * cw / a0, (1.0 - alpha) / a0]


def rbj_lpf(f0, fs, q):
    w0 = 2.0 * math.pi * f0 / fs
    cw = math.cos(w0)
    alpha = math.sin(w0) / (2.0 * q)
    a0 = 1.0 + alpha
    return [(1.0 - cw) * 0.5 / a0, (1.0 - cw) / a0, (1.0 - cw) * 0.5 / a0,
            -2.0 * cw / a0, (1.0 - alpha) / a0]


def band_sections(fs, lo=BAND_LO, hi=BAND_HI):
    """The band's filters: hp x2 then lp x2, designed at `fs`.

    `lo` and `hi` are the corners as DESIGNED.  What corner they land on
    depends on the rate the filters are actually run at - see odf_from_cache.
    """
    return [rbj_hpf(lo, fs, Q), rbj_hpf(lo, fs, Q),
            rbj_lpf(hi, fs, Q), rbj_lpf(hi, fs, Q)]


def apply_sections(x, sections):
    """The transposed DF-II the AR uses, applied as one cascade."""
    y = x.astype(np.float64)
    for b0, b1, b2, a1, a2 in sections:
        y = lfilter([b0, b1, b2], [1.0, a1, a2], y)
    return y


# --------------------------------------------------------------------------
# The ODF, the two ways.
# --------------------------------------------------------------------------

def odf_from_cache(x6k, band_fs, lo=BAND_LO, hi=BAND_HI):
    """The device's ODF from the 6 kHz cache, for a band designed at band_fs.

    band_fs is the rate the band filters are DESIGNED for, and the cache is
    at 6 kHz.  A filter designed for a corner f at a rate r lands at
    f*(run_rate/r) on the stream it runs on, so:
      band_fs = 6000, corners 90/320 -> 90 and 320 Hz  (the firmware)
      band_fs = 3000, corners 40/160 -> 80 and 320 Hz  (the corpus's)
    Either way the stream is decimated by two to 3 kHz and framed at 32.
    """
    y = apply_sections(x6k, band_sections(band_fs, lo, hi))
    y = y[::2]                                   # 6 kHz -> 3 kHz
    n = (len(y) // HOP) * HOP
    if n == 0:
        return np.zeros(0)
    p = (y[:n].reshape(-1, HOP) ** 2).mean(axis=1)
    db = 10.0 * np.log10(p + 1e-12)
    return np.maximum(np.diff(db, prepend=db[0]), 0.0)


# --------------------------------------------------------------------------
# The tracker.
# --------------------------------------------------------------------------

def rnd(x):
    """roundf(): half AWAY FROM ZERO, like C's roundf.

    Python's round() is ties-to-even, and the difference is not academic:
    the grid's phase anchor is a round() of (nextBeat - phase)/period, and
    a tie puts the port's grid a whole period - half a beat - away from
    where the board puts it.  The board's arithmetic is float32, so ties
    are more likely there than here.
    """
    return math.floor(x + 0.5) if x >= 0.0 else math.ceil(x - 0.5)


def narrow_window(bpm, half_bpm=2.0):
    """The narrow window, in candidate indices - arTempoNarrowWindow().

    The window is arithmetic in the CANDIDATE GRID, not in the BPM: the
    published rate is a float and it is rarely exactly on a candidate, so a
    window computed by rounding the two BPM edges loses a candidate at one
    end and comes out asymmetric - 124.9999 gave sixteen candidates where
    it should have given seventeen.  Snapping the centre to the grid and
    counting outwards gives the same window every time.
    """
    half = int(rnd(half_bpm / CAND_STEP))
    centre = int(rnd((bpm - BPM_LO) / CAND_STEP))
    f = centre - half
    c = 2 * half + 1
    if f < 0:
        c += f
        f = 0
    if f + c > CAND_COUNT:
        c = CAND_COUNT - f
    if f < 0:
        f = 0
    if c < 1:
        c = 1
    return f, c


class Tracker(object):
    """ar_tempo.h, in Python."""

    def __init__(self):
        self.hist = np.zeros(HIST_RING)
        self.hist_idx = 0
        self.hist_filled = 0

        self.peak_frame = []          # newest last
        self.prev_peak_frame = -1000
        self.prev_peak_str = 0.0

        self.odf_max = 0.0

        self.period = 0.0
        self.next_beat = 0.0
        self.beat_index = 0
        self.locked = False
        self.bpm = 0.0
        self.score = 0.0
        self.frame_index = 0
        self.beats = []

        # The rate, frame by frame.  The re-lock experiment reads it.
        self.bpm_log = []
        self.score_log = []

        # The work: how many candidates the comb has scored.  The C++ pays
        # for each one, so this is the CPU.
        self.cand_total = 0
        self.search_narrow = 0
        self.search_full = 0

        self.sweep_frames = None
        self.sweep_str = None
        self.sweep_index = 0
        self.sweep_now = 0.0
        self.sweep_active = False
        self.sweep_best_value = -1e9
        self.sweep_best_score = 0.0
        self.sweep_best_bpm = 0.0
        self.sweep_best_phase = 0.0
        # The FAR candidate: the best EVIDENCE outside the narrow window.
        # It is nominated by the RAW score, so the prior cannot choose which
        # far rate argues with the local one.
        self.sweep_far_score = -1e9
        self.sweep_far_bpm = 0.0
        self.sweep_far_phase = 0.0
        # A whole-band search that found nothing: the narrow search must not
        # ask for another one, or the sweep runs every ninth frame.
        self.full_search_failed = False
        self.collapse_count = 0

        # The prior's weight is a KNOB, because the prior is a tie-breaker
        # and not evidence - and a tie-breaker that can outvote a real
        # score difference is not a tie-breaker, it is an attractor.
        self.prior_weight = PRIOR_WEIGHT
        self.prior = self._prior_table()
        # Does the prior vote INSIDE the narrow search?  The narrow search
        # looks +-2 BPM around the rate the grid already believes, so the
        # prior can only be a tie-breaker there - and a tie-breaker that
        # can outvote a real score difference is an attractor, not a
        # tie-breaker.  The whole-band search is where the prior belongs:
        # that is the search that has to choose a rate from nothing.
        self.prior_in_narrow = False

        # THE LATTICE.  A candidate grid of 0.25 BPM has three points in
        # every whole BPM, so a tracker sitting on 128.0 has 127.75 and
        # 128.25 to wobble to - and it wobbles, because the comb's evidence
        # for those two is nearly the same as for 128.0.  That is the
        # 127.8 / 128.2 wander, and it is a property of the GRID, not of
        # the music.
        #
        # The lattice throws the quarter-BPM points away: only multiples of
        # 0.5 BPM are candidates at all.  A tempo of 128.0 then has nothing
        # to wobble to, and a tempo of 122.5 - a real one - is still a
        # candidate, because a half is on the lattice.
        #
        # `whole_bonus` is how much a HALF-integer candidate is penalised
        # against a whole one.  It is a flat penalty over the whole band -
        # no centre, no attractor - so unlike the prior it is safe to
        # apply inside the narrow search too.
        self.quant = 0                 # 0 = off, 1 = the half-BPM lattice
        self.whole_bonus = 0.0         # the penalty on a half-integer

        # The knobs the cost experiment turns. The defaults are the C++'s.
        self.delta = PEAK_DELTA
        self.floor = PEAK_FLOOR
        self.top_n = 0            # 0 = every peak in the window
        self.cand_step = CAND_STEP
        self.cand_count = CAND_COUNT

        # The narrow search: once the grid is running, the search does not
        # have to look at the whole band again.  It looks at the rate it
        # believes, plus or minus `narrow_bpm`, and it looks every
        # `search_every` frames.  The whole-band search is then only needed
        # to ACQUIRE, and after a track change.
        #
        # narrow_bpm = 0 turns it off: every search is a whole-band search.
        # The default is the firmware's: AR_TEMPO_NARROW_BPM.
        self.narrow_bpm = 2.0
        # The kernel's half-width: INTEGER (what the firmware does) or
        # CONTINUOUS (the same box with no rounding).
        # The EXACT box - no rounding of the half-width.  This is what the
        # firmware does; setting it False reproduces the OLD rounded box,
        # which is kept only so the two can be compared.
        self.exact_kernel = True
        self.search_every = 4
        self.last_sweep_frame = -1000
        # The safety net has its OWN clock.  Sharing the narrow search's
        # clock would mean the narrow search resets it every four frames
        # and the net is never due - which is exactly the bug the first
        # version of this file had, and exactly the bug the C++ avoided by
        # keeping `lastFullFrame` separate.
        self.last_full_frame = -1000
        self.sweep_bpms = None
        self.sweep_gidx = None
        self.sweep_best_index = 0
        self.need_full = False
        self.sweep_clipped_lo = False
        self.sweep_clipped_hi = False
        self.sweep_was_narrow = False

        # The three ways a narrow search can be told to look everywhere
        # instead.  Which one is right is measured, not guessed.
        #
        # EDGE: the narrow search's winner sits at the edge of the window,
        #       so the rate it is tracking may be beyond it.
        # COLLAPSE: the narrow search cannot find the beat any more - its
        #       score fell below the minimum - so the music changed.
        # SAFETY NET: a whole-band search every `full_every` frames,
        #       whatever the score says.
        #
        # THE DEFAULTS ARE THE SHIPPED FIRMWARE'S, so that a script which
        # does not set them measures the board and not some earlier idea of
        # it.  The EDGE rule is the exception: the firmware has no such
        # rule at all, so it is off.
        #
        # This mattered.  The band and the veto margin were chosen with
        # these defaults as they were - the clamp instead of the
        # proportional PLL, no narrow search and no veto at all - and
        # neither number survived being measured against the tracker that
        # actually ships.
        self.edge_rule = False
        self.collapse_trigger = True
        self.full_every = 32
        # WHY the next search has to be a whole-band one: None, 'net' (the
        # safety net) or 'collapse' (the narrow search lost the beat).
        self.need_full_reason = None
        self.sweep_reason = None

        # THE VETO.  The whole-band safety net finds the best rate in the
        # whole band; the narrow search finds the best rate around the grid.
        # They disagree sometimes, and when they do the grid is dragged
        # between them - which is what the board was doing, answering 125.00
        # and then 131.00.
        #
        # So the far rate may only move the grid if it is CLEARLY better
        # evidence than the local rate.  The two are measured on the SAME
        # onset snapshot, and on the RAW score: the prior is a tie-breaker,
        # not evidence, and letting it decide the fight is what made the
        # prior's centre (125.00) into an attractor.
        #
        # The margin is RELATIVE - a fraction of the local score, not an
        # absolute one.  MEASURED, on real house music:
        #
        #   two rates that are really the same music   1.00 .. 1.13
        #   two rates where the far one is really better 1.26 .. 1.50
        #   the music has changed and the window has turned over  1.23 .. 1.34
        #
        # so 0.20 sits in the gap: the near-ties are refused and the real
        # change is taken.
        #
        # 0 turns the veto off (the whole band always wins).
        # AR_TEMPO_NET_MARGIN.  The firmware has no "veto off" switch: at a
        # margin of zero the far rate is still refused unless it is
        # STRICTLY WORSE than the local one.  The port used to switch the
        # veto off entirely at a margin of zero, which is a different rule,
        # so a margin sweep measured the wrong thing.
        self.net_margin = 0.70
        self.net_vetoes = 0
        self.net_far = 0
        # A record of every whole-band sweep's decision, for the diagnostic.
        # Empty unless somebody asks for it.
        self.trace = []

        # Does the veto apply to the COLLAPSE whole-band search as well as
        # to the safety net's?  The two are different animals:
        #
        #   SAFETY NET  the grid is happy, the music may have changed.
        #                The far rate is a near-tie and the grid gets
        #                dragged - the 125/131 fight.  The veto is for this.
        #   COLLAPSE    the narrow search cannot find the beat any more.
        #                That is a STATEMENT that the old rate is gone, not
        #                a near-tie, and the far rate is the only evidence
        #                left.  The veto here can refuse a real change and
        #                leave the grid stuck on a dead rate.
        self.veto_on_collapse = True

        # When the far candidate WINS the fight, does the grid land ON the
        # new rate (hard) or walk toward it (soft)?  Walking passes through
        # the rates where nothing aligns at all, and the tracker sits in
        # that dead zone for seconds.
        self.hard_far = False

        # The PLL's rate response.  0 = the clamp the C++ has (at most
        # PLL_SLEW of the period per adopt); > 0 = proportional, the rate
        # moves that fraction of the way to the target.
        # The default is the firmware's: AR_TEMPO_PLL_GAIN.
        self.pll_gain = 0.20

        # THE KERNEL'S POLICY - see comb_scores_all().  'time' is the
        # firmware's own, and it is the one the shipped measurements were
        # made with.
        self.kernel_policy = 'minfrac'
        self.plateau = 'centre'        # 'lead' or 'centre'
        self.min_score = MIN_SCORE
        # The collapse rule: how many searches in a row under the floor it
        # takes to call the grid dead, and how much better the far rate has
        # to be before a DEAD local rate gives up the grid to it.
        #
        # The margin is the load-bearing part.  The rule used to be "the
        # local rate is under the floor, so take the far rate" - and it took
        # the far rate even when the far rate was WORSE.  MEASURED on a
        # DJ-style mix of twelve tracks: 20.4 % of the mix above 140 BPM
        # (the music never is) became 0.0 %, and the mean error against
        # the truth went from 6.60 to 2.11 BPM.
        self.collapse_count_max = COLLAPSE_COUNT
        self.dead_margin = 0.05
        self.kernel_frac = 0.14          # 'frac': the half-width, as a
                                          # fraction of the period

    # ---- the comb ------------------------------------------------------

    def comb_score(self, frames, strengths, bpm):
        """(score, phase in frames) - arTempoCombScore()."""
        s, p = self.comb_scores_all(frames, strengths, np.array([bpm]))
        return float(s[0]), float(p[0])

    def comb_scores_all(self, frames, strengths, bpms):
        """The comb at MANY candidate rates at once.  (scores, phases).

        The C++ scores 44 candidates a frame so the cost is flat; the RESULT
        is the same set of scores, so the port can take them all at once.

        The PHASE is computed in float32, because the firmware computes it in
        float32 and the difference is not academic: the phase decides which
        bin an onset lands in, and an onset that sits within a thousandth of
        a bin edge lands in a DIFFERENT bin on the P4 than it does here.
        One onset moved between neighbouring bins moves the score by up to
        1.7 % - which is a tenth of the veto's margin.

        The histogram and the box stay in float64.  Their rounding is a
        hundredth of that, and the firmware's own arithmetic there is a
        rolling float32 sum, which no vectorised form reproduces exactly
        anyway.
        """
        n = len(frames)
        total = float(np.sum(strengths))
        c = len(bpms)
        if total <= 0 or n == 0:
            return np.zeros(c), np.zeros(c)
        f32 = np.float32
        # the firmware's own arithmetic: the period, its reciprocal, and
        # the phase offset, all in float32
        periods = (f32(60.0) * f32(RATE)
                   / np.asarray(bpms, dtype=f32)).astype(f32)
        inv = (f32(1.0) / periods).astype(f32)
        fr = np.asarray(frames, dtype=f32)
        # the C++'s phase: a FLOOR of the quotient, not a truncation, and
        # then the two corrections for a multiply that rounded across an
        # integer
        k = np.floor(fr[None, :] * inv[:, None]).astype(f32)
        u = (fr[None, :] - k * periods[:, None]).astype(f32)
        u = np.where(u < f32(0.0), u + periods[:, None], u).astype(f32)
        u = np.where(u >= periods[:, None], u - periods[:, None], u).astype(f32)
        bin_scale = (f32(COMB_BINS) * inv).astype(f32)
        bins = np.clip((u * bin_scale[:, None]).astype(np.int64), 0, COMB_BINS - 1)
        # the histogram, one row per candidate
        flat = (np.repeat(np.arange(c), n) * COMB_BINS + bins.ravel())
        h = np.bincount(flat, weights=np.tile(strengths, c),
                        minlength=c * COMB_BINS).reshape(c, COMB_BINS)
        # The kernel's half-width, in bins.
        #
        # It used to be ROUNDED to a whole number of bins, and a bin is
        # period/64 wide - 7 ms at 128 BPM - so the tolerance the tracker
        # actually applied moved by up to half a bin from one candidate to
        # the next.  A WIDER box catches more of the smear of a wrong
        # period, so the candidate that happened to round up scored
        # higher, and the score profile had a sawtooth on it.
        #
        # With `exact_kernel` the rounding is gone: the box is the EXACT
        # integral of the histogram over the requested half-width.  The
        # integer rolling sum is still there - it is the box's whole part -
        # and the two fractional edge bins are added on.
        tol_frames = f32((TOL_MS * 0.001) * RATE)
        wf = (tol_frames / periods * f32(COMB_BINS)).astype(f32)
        # THE KERNEL'S POLICY - what the box's half-width means.
        #
        # 'time' (the firmware today) is a box of a FIXED WIDTH IN TIME, so
        # a fast candidate's box covers a larger FRACTION of its period.
        # That is a bias, and it is measured: on 25 real house tracks the
        # raw score's argmax is at 169 BPM median while the music is at 128.
        #
        # 'frac'  the box is a fixed FRACTION of the period - it can never
        #         merge two beat clusters, because the closest they can get
        #         is a third of a period apart.
        # 'min'   the tolerance, but never wider than a sixth of the period
        #         (the same no-merge rule, but it keeps the tolerance
        #         wherever the tolerance fits).
        # 'dens'  the box's MEAN per bin instead of its sum: the score stops
        #         rewarding a candidate for having a wider box.
        # 'exc'   the capture above the capture a uniform phase would give.
        pol = self.kernel_policy
        if pol == 'frac':
            wf = np.full(len(periods), self.kernel_frac * COMB_BINS, dtype=f32)
        elif pol == 'min':
            wf = np.minimum(wf, f32(COMB_BINS) / f32(6.0))
        elif pol == 'minfrac':
            wf = np.minimum(wf, f32(self.kernel_frac * COMB_BINS))
        elif pol == 'dens':
            pass
        elif pol == 'exc':
            pass
        else:
            pass
        # the rolling window, by the same cumsum trick the C++'s rolling sum
        # is equivalent to
        h3 = np.concatenate((h, h, h), axis=1)
        pre = np.concatenate((np.zeros((c, 1)), np.cumsum(h3, axis=1)), axis=1)
        i = np.arange(COMB_BINS)
        if not self.exact_kernel:
            # the OLD, rounded box - kept so the two can be compared
            w = np.clip(np.rint(wf).astype(int), 1, COMB_BINS // 2 - 1)
            hi = COMB_BINS + i[None, :] + w[:, None] + 1
            lo = COMB_BINS + i[None, :] - w[:, None]
            win = (np.take_along_axis(pre, hi, axis=1)
                   - np.take_along_axis(pre, lo, axis=1))
        else:
            # The EXACT box.
            #
            # The box is centred on the BIN's CENTRE, (i + 1/2), because
            # the integer box of half-width k covers bins i-k .. i+k, and
            # that is the continuous interval [i-k, i+k+1), whose centre
            # is i+1/2.  Write the requested half-width as wf = k + f:
            #
            #   f < 0.5   the exact box is the integer one MINUS (0.5+f)
            #             of each of its two edge bins - it is narrower
            #   f > 0.5   the exact box is the integer one PLUS (f-0.5)
            #             of each of the bins just OUTSIDE it - it is
            #             wider
            #
            # At f = 0.5 the two agree, and that is the point where the
            # integer box IS the exact box.
            # The half-width is clamped BEFORE it is split, so the
            # remainder is always in [0,1).  Clamping the integer part
            # instead would leave the remainder above 1 whenever the
            # clamp bit, and the edge rule - which assumes a remainder
            # of less than one bin - would add more than a whole bin's
            # worth of the bins outside the box.
            wf = np.clip(wf, f32(1.0), f32(COMB_BINS // 2 - 2)).astype(f32)
            k = np.floor(wf).astype(int)
            f = (wf - k.astype(f32)).astype(f32)
            hi = COMB_BINS + i[None, :] + k[:, None] + 1
            lo = COMB_BINS + i[None, :] - k[:, None]
            win = (np.take_along_axis(pre, hi, axis=1)
                   - np.take_along_axis(pre, lo, axis=1))
            kk = k[:, None]
            ff = f[:, None]
            narrow = (ff < 0.5)
            idx_lo = COMB_BINS + i[None, :] - kk
            idx_hi = COMB_BINS + i[None, :] + kk
            # narrow -> the box's own edge bins; wide -> the ones just out
            lo_i = np.where(narrow, idx_lo, idx_lo - 1)
            hi_i = np.where(narrow, idx_hi, idx_hi + 1)
            h_lo = np.take_along_axis(h3, np.clip(lo_i, 0, h3.shape[1] - 1), axis=1)
            h_hi = np.take_along_axis(h3, np.clip(hi_i, 0, h3.shape[1] - 1), axis=1)
            # how much of each edge bin the box drops (narrow) or takes up
            # (wide).  For the narrow case the box gives back (0.5-f) of
            # each of its own edge bins: the integer box covers bins
            # i-k .. i+k, and the exact box stops half a bin short of
            # each end.
            amt = np.where(narrow, 0.5 - ff, ff - 0.5)
            sgn = np.where(narrow, -1.0, 1.0)
            win = win + sgn * amt * (h_lo + h_hi)
        j = np.argmax(win, axis=1)
        best = win[np.arange(c), j]
        centre_bin = j.astype(np.float64)
        if self.plateau == 'centre':
            # THE PLATEAU'S CENTRE, not its leading edge.
            #
            # The box is 2*w+1 bins wide, so a cluster NARROWER than the
            # box produces a plateau of equal values, and the argmax returns
            # the FIRST of them - up to a whole tolerance BEFORE the
            # cluster's centre.  The grid is anchored on that phase, so
            # every beat would be up to 70 ms early, and a constant offset
            # is invisible to a metric that removes the offset.
            #
            # The array is tripled so the plateau can wrap round the circle:
            # a run that leaves bin 63 continues into bin 0.
            #
            # The plateau is the run of equal values that CONTAINS the
            # argmax, so it is found by the last bin below the peak BEFORE
            # the argmax and the first one after it.  (A prefix product
            # finds the first near bin in the array, not the first near bin
            # of the run, and it silently returns the argmax - which is
            # exactly the bug this is here to fix.)
            #
            # The comparison is RELATIVE, like the firmware's 1e-5, because
            # the rolling sum is float32 and the plateau's values drift in
            # the last bits.
            mx = best[:, None]
            near = win >= (mx - np.maximum(1e-12, mx * 1e-5))
            near3 = np.concatenate((near, near, near), axis=1)
            ar = np.arange(near3.shape[1])
            isfalse = ~near3
            last_false = np.maximum.accumulate(np.where(isfalse, ar, -1), axis=1)
            rev = np.where(isfalse, ar, near3.shape[1])
            next_false = np.minimum.accumulate(rev[:, ::-1], axis=1)[:, ::-1]
            rows = np.arange(c)
            mid = COMB_BINS + j
            a = last_false[rows, mid] + 1
            b = next_false[rows, mid] - 1
            # the plateau's centre, in bin units: the bins' LEFT edges are
            # the positions, and the box is centred on a bin's CENTRE, so a
            # plateau of bins a..b has its centre at (a+b)/2 + 1/2
            centre_bin = (a + b) / 2.0 + 0.5 - COMB_BINS
        if pol == 'dens':
            # the box's MEAN per bin, over the total: a candidate with a
            # wide box no longer gets credit for the width
            wbins = np.maximum(1.0, 2.0 * wf + 1.0)
            score = best / (wbins * total)
        elif pol == 'exc':
            # the capture ABOVE the capture a uniform phase would give
            chance = np.minimum(1.0, 2.0 * wf / f32(COMB_BINS))
            score = (best - chance * total) / np.maximum(1e-6, (1.0 - chance) * total)
        else:
            score = best / total
        # The PHASE VALUE is float32 too.  The firmware computes it as
        # (float)bestBin / (float)AR_TEMPO_COMB_BINS * period, and the phase
        # is what the grid is anchored on, so a float64 phase here and a
        # float32 phase there can put the port's beat a whole period away
        # from the board's.
        phase = (centre_bin.astype(np.float32) / np.float32(COMB_BINS)).astype(np.float32) \
            * periods.astype(np.float32)
        phase = phase.astype(np.float64)
        return score, phase

    @staticmethod
    def _cint(h3, pre, x):
        """The integral of the piecewise-constant histogram up to x."""
        j = np.floor(x).astype(int)
        f = x - j
        rows = np.arange(h3.shape[0])[:, None]
        return (np.take_along_axis(pre, j, axis=1)
                + f * h3[rows, j])

    # ---- the picker ----------------------------------------------------

    def pick_peak(self, odf, frame):
        self.hist[self.hist_idx] = odf
        self.hist_idx = (self.hist_idx + 1) % HIST_RING
        if self.hist_filled < HIST_RING:
            self.hist_filled += 1
        if self.hist_filled < HIST_LEN + PEAK_POST + 1:
            return
        head = (self.hist_idx + HIST_RING - HIST_LEN) % HIST_RING
        h = self.hist[(head + _HIST_I) & (HIST_RING - 1)]
        base = HIST_LEN - 1 - PEAK_POST
        oj = h[base]
        if oj <= 0.0:
            return
        if oj < h[base - PEAK_PRE: base + PEAK_POST + 1].max():
            return
        mean = float(h[base - PEAK_MEAN_HALF: base + 1].mean())
        if oj < (1.0 + self.delta) * mean:
            return
        on = (oj / self.odf_max) if self.odf_max > 0 else 0.0
        if on < self.floor:
            return
        j = frame - PEAK_POST
        if (j - self.prev_peak_frame) < PEAK_MIN_GAP:
            if on > self.prev_peak_str:
                self.prev_peak_frame = j
                self.prev_peak_str = on
                self.peak_frame[-1] = (j, on)
            return
        self.prev_peak_frame = j
        self.prev_peak_str = on
        self.peak_frame.append((j, on))
        if len(self.peak_frame) > PEAK_RING:
            self.peak_frame.pop(0)

    # ---- the PLL -------------------------------------------------------

    def adopt(self, bpm, phase, now, hard=False):
        P = 60.0 * RATE / bpm
        if not self.locked:
            self.period = P
            self.next_beat = phase + rnd((now - phase) / P) * P
            self.locked = True
            self.beat_index = 0
        else:
            if hard:
                # RE-ANCHOR.  The evidence says the rate is a DIFFERENT rate,
                # not a slightly different one, so the grid is put ON the
                # new rate instead of being walked toward it.
                #
                # The walk is what does the damage: a ten per cent step from
                # 130 towards 173 passes through 140 and 150, where nothing
                # aligns at all, and the tracker sits in that dead zone for
                # seconds while its score is rubbish and the far candidate
                # keeps being re-nominated.
                self.period = P
            elif self.pll_gain > 0.0:
                # PROPORTIONAL.  The rate moves a fraction of the way to the
                # target, so a comb winner that flips between two candidates
                # a quarter of a BPM apart moves the rate by a quarter of
                # that - and a real change is taken in a handful of adopts.
                self.period += self.pll_gain * (P - self.period)
            else:
                # The clamp: at most PLL_SLEW of the period per adopt.
                lim = PLL_SLEW * self.period
                d = P - self.period
                self.period += lim if d > lim else (-lim if d < -lim else d)
            k = rnd((self.next_beat - phase) / self.period)
            anchor = phase + k * self.period
            if hard:
                self.next_beat = anchor
            else:
                self.next_beat += PLL_GAMMA * (anchor - self.next_beat)
        self.bpm = 60.0 * RATE / self.period

    def _prior_table(self):
        """The prior's penalty, one per candidate.

        The weight is applied here and in the narrow search's own penalty,
        so changing `prior_weight` and rebuilding the table changes both.
        """
        return np.array([
            self.prior_weight
            * (math.log((BPM_LO + i * CAND_STEP) / PRIOR_BPM)
               / PRIOR_SIGMA) ** 2
            for i in range(CAND_COUNT)])

    # ---- the sweep -----------------------------------------------------

    def sweep_start(self):
        cutoff = self.frame_index - COMB_FRAMES
        keep = [(f, s) for (f, s) in self.peak_frame if f >= cutoff]
        # The cap: the STRONGEST peaks in the window. The comb does not care
        # about the order - the modulo is order-independent - so the cap is
        # just a sort.
        if self.top_n and len(keep) > self.top_n:
            keep = sorted(keep, key=lambda t: -t[1])[:self.top_n]
        self.sweep_frames = np.array([f for f, _ in keep], dtype=float)
        self.sweep_str = np.array([s for _, s in keep], dtype=float)
        self.sweep_index = 0
        self.sweep_now = float(self.frame_index)
        self.last_sweep_frame = self.frame_index
        # The flag is read BEFORE it is cleared: it is the narrow search's
        # way of saying "the next search has to be a whole-band one".
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
        self.sweep_took_far = False
        # the narrow window's best, measured on the SAME snapshot as the
        # whole band, so the two are comparable
        self.sweep_narrow_first = -1
        self.sweep_narrow_count = 0
        self.sweep_narrow_score = -1e9
        self.sweep_narrow_bpm = 0.0
        self.sweep_narrow_phase = 0.0
        # the far candidate: the best evidence OUTSIDE the window
        self.sweep_far_score = -1e9
        self.sweep_far_bpm = 0.0
        self.sweep_far_phase = 0.0
        self.sweep_active = len(keep) >= MIN_PEAKS
        # The candidate set.  Locked and the narrow search is on, the search
        # looks only at the rate the grid believes, plus or minus the
        # half-width: the whole band is only for ACQUIRING.
        if narrow:
            lo = max(BPM_LO, self.bpm - self.narrow_bpm)
            hi = min(BPM_HI, self.bpm + self.narrow_bpm)
            self.sweep_clipped_lo = (lo <= BPM_LO)
            self.sweep_clipped_hi = (hi >= BPM_HI)
            # The C++'s candidates are on the GLOBAL grid - the index is
            # rounded to the step - so the narrow window is a set of whole
            # candidates and not a window with a fractional edge.
            f, c = narrow_window(self.bpm, self.narrow_bpm)
            gidx = np.arange(f, f + c)
        else:
            self.sweep_clipped_lo = False
            self.sweep_clipped_hi = False
            gidx = np.arange(self.cand_count)
        # THE LATTICE: keep only the candidates on the half-BPM grid.  The
        # window is centred, so the lattice inside it is symmetric too.
        if self.quant:
            gidx = gidx[(gidx % 2) == 0]
        bpms = BPM_LO + gidx * self.cand_step
        self.sweep_gidx = gidx
        self.sweep_bpms = bpms
        # All the candidates, scored at once from the snapshot. The C++ scores
        # 44 a frame so the cost is flat; the RESULT is the same set of
        # scores, so the port can take them together.
        self.cand_total += len(bpms)
        if narrow:
            self.search_narrow += 1
        else:
            self.search_full += 1
        if self.sweep_active:
            if narrow:
                if self.prior_in_narrow:
                    pen = np.array([
                        self.prior_weight
                        * (math.log(b / PRIOR_BPM) / PRIOR_SIGMA) ** 2
                        for b in bpms])
                else:
                    pen = np.zeros(len(bpms))
            else:
                # The prior is indexed by the CANDIDATE INDEX, so the
                # lattice has to index it too.
                pen = self.prior[self.sweep_gidx]
            scores, phases = self.comb_scores_all(self.sweep_frames,
                                                  self.sweep_str, bpms)
            # The whole-number preference: a flat penalty on the HALF
            # integers, so a whole BPM wins a near-tie against 127.5 and
            # 122.5 is not punished at all.
            wpen = 0.0
            if self.quant and self.whole_bonus:
                wpen = np.where((self.sweep_gidx % 4) == 2,
                                self.whole_bonus, 0.0)
            # The FIGHT's value: the evidence, less the lattice
            # preference.  The prior is NOT in it - the prior is a
            # tie-breaker and it must not choose which far rate gets to
            # argue with the local one.
            pick = scores - wpen
            values = pick - pen
            j = int(np.argmax(values))
            # THE VETO.  A whole-band search while the grid is running has
            # to find the best rate INSIDE the narrow window AND the best
            # rate OUTSIDE it - the same snapshot, so the two are
            # comparable - and the far one may only move the grid if it is
            # clearly better evidence than the local one.
            if (not narrow) and self.locked and self.narrow_bpm > 0.0 \
                    and (self.veto_on_collapse
                         or self.sweep_reason != 'collapse'):
                lo = max(BPM_LO, self.bpm - self.narrow_bpm)
                hi = min(BPM_HI, self.bpm + self.narrow_bpm)
                f, c = narrow_window(self.bpm, self.narrow_bpm)
                self.sweep_narrow_first = f
                self.sweep_narrow_count = c
                idx = np.where((self.sweep_gidx >= f)
                               & (self.sweep_gidx < f + c))[0]
                # The local reference is the best EVIDENCE in the window -
                # the raw score - and not the best value.  The prior is a
                # tie-breaker and it stays out of this fight.
                jn = int(idx[np.argmax(pick[idx])])
                self.sweep_narrow_score = float(scores[jn])
                self.sweep_narrow_bpm = float(bpms[jn])
                self.sweep_narrow_phase = float(phases[jn])
                # The FAR candidate: the best EVIDENCE outside the window,
                # nominated by the RAW score.  The prior used to choose it,
                # and that is what made 125.00 an attractor: a rate a long
                # way off never got to argue, because the prior had already
                # decided it was not worth mentioning.
                outside = np.ones(len(bpms), dtype=bool)
                outside[idx] = False
                jf = -1
                if outside.any():
                    jf = int(np.argmax(np.where(outside, pick, -1.0)))
                    self.sweep_far_score = float(scores[jf])
                    self.sweep_far_bpm = float(bpms[jf])
                    self.sweep_far_phase = float(phases[jf])
                # THE FIGHT.  Both sides are the raw score, on the same
                # snapshot, with the same kernel.
                #
                # The local rate has to be ALIVE to defend itself: a rate
                # whose score is below the minimum is one the tracker has
                # already declared dead, and it has no evidence to veto
                # with.
                # The local rate is ALIVE while its collapse is young.  A
                # single search under the floor is a dip, not a collapse:
                # the score falls under the floor on 0.7 % of the frames
                # of real house music, and handing the grid to the far
                # candidate on one of them is how a good rate turns into
                # 160.
                local_alive = self.collapse_count < self.collapse_count_max
                far_alive = self.sweep_far_score >= self.min_score
                if not local_alive:
                    take_far = (far_alive
                                and self.sweep_far_score
                                >= self.sweep_narrow_score * (1.0 + self.dead_margin))
                else:
                    take_far = (self.sweep_far_score
                                >= self.sweep_narrow_score * (1.0 + self.net_margin))
                if take_far and jf >= 0:
                    self.net_far += 1
                    j = jf
                    self.sweep_took_far = True
                else:
                    self.net_vetoes += 1
                    j = jn
                    self.sweep_took_far = False
                if self.trace is not None:
                    self.trace.append({
                        "frame": self.frame_index,
                        "reason": self.sweep_reason,
                        "bpm": self.bpm,
                        "local_bpm": self.sweep_narrow_bpm,
                        "local_score": self.sweep_narrow_score,
                        "far_bpm": self.sweep_far_bpm,
                        "far_score": self.sweep_far_score,
                        "ratio": (self.sweep_far_score
                                  / max(self.sweep_narrow_score, 1e-9)),
                        "took_far": bool(take_far and jf >= 0),
                        "peaks": len(self.sweep_frames),
                    })
            self.sweep_best_value = float(values[j])
            self.sweep_best_score = float(scores[j])
            self.sweep_best_bpm = float(bpms[j])
            self.sweep_best_phase = float(phases[j])
            self.sweep_best_index = j

    def sweep_step(self):
        if not self.sweep_active:
            return
        # The C++ counts CANDIDATES PER FRAME in `i` and the candidates done
        # in `sweepIndex` - they are different counters, and conflating them
        # means the sweep never finishes.
        total = len(self.sweep_bpms) if self.sweep_bpms is not None \
            else self.cand_count
        self.sweep_index = min(total, self.sweep_index + CAND_PER_FRAME)
        if self.sweep_index < total:
            return
        self.score = self.sweep_best_score
        if self.sweep_best_score >= self.min_score:
            self.full_search_failed = False
            self.collapse_count = 0
            self.adopt(self.sweep_best_bpm, self.sweep_best_phase, self.sweep_now,
                       hard=(self.hard_far and self.sweep_took_far))
        elif self.sweep_was_narrow:
            # The narrow search cannot find the beat any more.  Its score
            # fell below the minimum, so the music it was tracking is not
            # there: ask for a whole-band search.
            #
            # Unless a WHOLE-BAND search has already looked everywhere and
            # found nothing.  Then looking everywhere again will not make a
            # beat appear, and the sweep would run every ninth frame for
            # the price of 341 candidates.  The safety net still looks,
            # once every `full_every` frames - that is what it is for.
            self.collapse_count = min(255, self.collapse_count + 1)
            if (self.collapse_trigger and not self.full_search_failed
                    and self.collapse_count >= self.collapse_count_max):
                self.need_full = True
                self.need_full_reason = 'collapse'
        else:
            # A whole-band search that found nothing.  It has already
            # looked everywhere, so the narrow search must not ask it to.
            self.full_search_failed = True
            self.collapse_count = min(255, self.collapse_count + 1)
        # The narrow search that ran out of room: the winner is at the edge
        # of the window, so the next search is a whole-band one.  The grid
        # itself is not thrown away.  An edge that is the BAND's edge is not
        # the search's fault and does not ask for one.
        if self.edge_rule and total < self.cand_count:
            at_lo = (self.sweep_best_index == 0) and not self.sweep_clipped_lo
            at_hi = (self.sweep_best_index == total - 1) \
                and not self.sweep_clipped_hi
            if at_lo or at_hi:
                self.need_full = True
        self.sweep_active = False

    # ---- the frame -----------------------------------------------------

    def frame(self, odf):
        self.frame_index += 1
        if odf > self.odf_max:
            self.odf_max = odf
        else:
            self.odf_max *= MAX_DECAY
        self.pick_peak(odf, self.frame_index)
        if self.locked:
            while self.frame_index >= self.next_beat:
                self.beats.append(self.next_beat)
                self.next_beat += self.period
                self.beat_index += 1
        self.bpm_log.append(self.bpm)
        self.score_log.append(self.score)
        if not self.sweep_active:
            # The safety net: a whole-band search every so often, whatever
            # the score says.  It is the belt to the narrow search's braces:
            # a music change that does not collapse the score would
            # otherwise never be noticed.
            if (self.full_every
                    and self.frame_index - self.last_full_frame >= self.full_every):
                self.need_full = True
                self.need_full_reason = 'net'
            # Unlocked, the search runs as soon as the last one finished: it
            # is looking for the rate and there is no rate to look around.
            # Locked, it runs every `search_every` frames - the estimate is
            # good enough to be searched around.
            if (not self.locked
                    or self.need_full
                    or self.frame_index - self.last_sweep_frame >= self.search_every):
                self.sweep_start()
        self.sweep_step()

    def run(self, odf):
        """The whole tracker over one ODF."""
        for i in range(len(odf)):
            self.frame(float(odf[i]))
        return self


# --------------------------------------------------------------------------
# The metric.
# --------------------------------------------------------------------------

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
    ap.add_argument("--limit", type=int, default=300)
    ap.add_argument("--show", type=int, default=14)
    args = ap.parse_args()

    keys = _cache.keys()[:args.limit]
    res = {"fw": [], "off": []}
    rows = []
    t0 = time.time()
    for k in keys:
        x = _cache.load(k)
        if x is None:
            continue
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        if len(ons) < 4:
            continue
        row = [k]
        for tag, band_fs, lo, hi in (("fw", 6000.0, BAND_LO, BAND_HI),
                                     ("off", 3000.0, 40.0, 160.0)):
            odf = odf_from_cache(x, band_fs, lo, hi)
            if len(odf) < 1200:
                row.append(0.0)
                row.append(0.0)
                continue
            t = Tracker()
            t.run(odf)
            f, off = hit_with_offset(np.array(t.beats) / RATE, ons)
            res[tag].append(f)
            row += [f, t.bpm]
        rows.append(row)
        if len(rows) % 20 == 0:
            print("  %d/%d  (%.0f s)" % (len(rows), len(keys), time.time() - t0),
                  flush=True)

    a = np.array(res["fw"])
    b = np.array(res["off"])
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    print()
    print("%d windows   %.0f s" % (len(rows), time.time() - t0))
    print()
    print("THE C++'S BAND (90-320 Hz, designed at 6 kHz then decimated)")
    print("  median %5.3f   >=0.5: %5.1f%%   >=0.8: %5.1f%%"
          % (float(np.median(a)), 100.0 * float((a >= 0.5).mean()),
             100.0 * float((a >= 0.8).mean())))
    print("THE OFFLINE BAND (80-320 Hz - the filters designed at 3 kHz)")
    print("  median %5.3f   >=0.5: %5.1f%%   >=0.8: %5.1f%%"
          % (float(np.median(b)), 100.0 * float((b >= 0.5).mean()),
             100.0 * float((b >= 0.8).mean())))
    d = a - b
    print()
    print("the firmware band is better on %d of %d windows (%.1f%%)"
          % (int((d > 0).sum()), n, 100.0 * float((d > 0).mean())))

    rows.sort(key=lambda r: r[1])
    print()
    print("the worst windows, by the firmware band's hit rate:")
    for r in rows[:args.show]:
        print("  %-42s  fw %5.3f (%7.2f)   off %5.3f (%7.2f)"
              % (r[0][:42], r[1], r[2], r[3], r[4]))


if __name__ == "__main__":
    sys.exit(main())

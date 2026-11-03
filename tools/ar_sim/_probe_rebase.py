#!/usr/bin/env python3
"""A faithful transcription of ar_tempo.h's frame machinery, float32 and all.

The point is the REBASE path: the shipped firmware rebases the frame counter
every AR_TEMPO_REBASE_FRAMES, and the offline port (_fw_port.py) has no rebase
at all, so nothing offline has ever exercised it.

Every line here is a line of usermods/audioreactive/ar_tempo.h.  f32() is the
float32 the P4 uses.
"""
import math
import numpy as np

f32 = np.float32


def f(x):
    return np.float32(x)


def s32(v):
    """Read a 32-bit pattern back as a signed int32 - the C++'s cast."""
    v &= 0xFFFFFFFF
    return v - 0x100000000 if v >= 0x80000000 else v


# ---- constants (ar_tempo.h) -----------------------------------------------
RATE = f(93.75)
DEC = 16
HOP = 32

PEAK_PRE, PEAK_POST = 3, 3
PEAK_MEAN_HALF = 33
PEAK_DELTA = f(0.35)
PEAK_FLOOR = f(0.06)
PEAK_MIN_GAP = 5
HIST_LEN = 37
HIST_RING = 64
MAX_DECAY = f(math.exp(-1.0 / (5.0 * 93.75)))

BPM_LO, BPM_HI = 90.0, 175.0
CAND_STEP = 0.25
CAND_COUNT = 341
COMB_BINS = 64
CAND_PER_FRAME = 44
COMB_FRAMES = 1125

PRIOR_BPM, PRIOR_SIGMA, PRIOR_WEIGHT = 125.0, 0.35, 0.6
MIN_SCORE = f(0.10)
MIN_PEAKS = 8

PLL_GAMMA = f(0.35)
PLL_GAIN = f(0.10)

NARROW_BPM = 2.0
SEARCH_EVERY = 4
FULL_EVERY = 32
NET_MARGIN = f(0.20)

PEAK_RING = 256
TOL_MS = 70.0

PRIOR = np.array([PRIOR_WEIGHT * (math.log((BPM_LO + i * CAND_STEP) / PRIOR_BPM)
                                   / PRIOR_SIGMA) ** 2
                  for i in range(CAND_COUNT)], dtype=np.float32)


class Tempo:
    def __init__(self, rebase=65536, tol_ms=TOL_MS):
        self.rebase = rebase
        self.tol_ms = tol_ms
        self.hist = np.zeros(HIST_RING, dtype=np.float32)
        self.hist_idx = 0
        self.hist_filled = 0
        self.peak_frame = [0] * PEAK_RING
        self.peak_str = [f(0.0)] * PEAK_RING
        self.peak_head = 0
        self.peak_count = 0
        self.prev_peak_frame = -1000
        self.prev_peak_str = f(0.0)
        self.odf_max = f(0.0)
        self.frame_index = 0
        self.abs_frame = 0
        self.period = f(0.0)
        self.next_beat = f(0.0)
        self.last_beat = f(0.0)
        self.beat_index = 0
        self.locked = False
        self.bpm = f(0.0)
        self.score = f(0.0)
        self.last_sweep_frame = 0
        self.last_full_frame = 0
        self.sweep_active = False
        self.sweep_was_narrow = False
        self.need_full = False
        self.full_search_failed = False
        self.sweep_now = f(0.0)
        self.sweep_total = f(0.0)
        self.sweep_frames = []
        self.sweep_strs = []
        self.sweep_index = 0
        self.sweep_first = 0
        self.sweep_n = CAND_COUNT
        self.sweep_best_value = f(-1e9)
        self.sweep_best_score = f(0.0)
        self.sweep_best_bpm = f(0.0)
        self.sweep_best_phase = f(0.0)
        self.sweep_best_idx = 0
        self.sweep_has_narrow = False
        self.sweep_narrow_first = 0
        self.sweep_narrow_count = 0
        self.sweep_narrow_score = f(-1e9)
        self.sweep_narrow_value = f(-1e9)
        self.sweep_narrow_bpm = f(0.0)
        self.sweep_narrow_phase = f(0.0)
        self.sweep_narrow_idx = 0
        self.sweep_far_score = f(-1e9)
        self.sweep_far_value = f(-1e9)
        self.sweep_far_bpm = f(0.0)
        self.sweep_far_phase = f(0.0)
        self.sweep_far_idx = 0
        self.log = []          # (frame_index, next_beat, last_beat, bpm, score)

    # ---- the comb (arTempoCombScore) -------------------------------------
    def kernel_half_width(self, period):
        tol_frames = f(f(self.tol_ms * 0.001) * RATE)
        wf = f(f(tol_frames / period) * f(COMB_BINS))
        wmax = f(COMB_BINS // 2 - 2)
        if wf < f(1.0):
            wf = f(1.0)
        if wf > wmax:
            wf = wmax
        w = int(wf)
        return w, f(wf - f(w))

    def comb_score(self, bpm):
        period = f(f(f(60.0) * RATE) / f(bpm))
        inv = f(f(1.0) / period)
        bin_scale = f(f(COMB_BINS) * inv)
        h = [f(0.0)] * COMB_BINS
        for fr, st in zip(self.sweep_frames, self.sweep_strs):
            ff = f(fr)
            k = f(math.floor(f(ff * inv)))
            u = f(ff - f(k * period))
            if u < f(0.0):
                u = f(u + period)
            elif u >= period:
                u = f(u - period)
            b = int(f(u * bin_scale))
            if b < 0:
                b = 0
            elif b >= COMB_BINS:
                b = COMB_BINS - 1
            h[b] = f(h[b] + st)
        total = self.sweep_total
        if total <= f(0.0):
            return f(0.0), f(0.0)
        w, fr_ = self.kernel_half_width(period)
        win = f(0.0)
        for j in range(-w, w + 1):
            win = f(win + h[(j + COMB_BINS) & (COMB_BINS - 1)])
        best = f(-1.0e30)
        best_bin = 0
        for i in range(COMB_BINS):
            if i > 0:
                win = f(win + h[(i + w) & (COMB_BINS - 1)]
                        - h[(i - w - 1 + COMB_BINS) & (COMB_BINS - 1)])
            v = win
            if fr_ < f(0.5):
                v = f(v - f(f(f(0.5) - fr_) *
                            f(h[(i - w + COMB_BINS) & (COMB_BINS - 1)]
                              + h[(i + w) & (COMB_BINS - 1)])))
            else:
                v = f(v + f(f(fr_ - f(0.5)) *
                            f(h[(i - w - 1 + COMB_BINS) & (COMB_BINS - 1)]
                              + h[(i + w + 1) & (COMB_BINS - 1)])))
            if v > best:
                best = v
                best_bin = i
        phase = f(f(f(best_bin) / f(COMB_BINS)) * period)
        return f(best / total), phase

    # ---- the narrow window (arTempoNarrowWindow) --------------------------
    def narrow_window(self, bpm):
        half = int(round(NARROW_BPM / CAND_STEP))
        centre = int(round((bpm - BPM_LO) / CAND_STEP))
        fi = centre - half
        c = 2 * half + 1
        if fi < 0:
            c += fi
            fi = 0
        if fi + c > CAND_COUNT:
            c = CAND_COUNT - fi
        if fi < 0:
            fi = 0
        if c < 1:
            c = 1
        return fi, c

    # ---- the picker (arTempoPickPeak) -------------------------------------
    def pick_peak(self, odf, frame):
        self.hist[self.hist_idx] = odf
        self.hist_idx = (self.hist_idx + 1) & (HIST_RING - 1)
        if self.hist_filled < HIST_RING:
            self.hist_filled += 1
        if self.hist_filled < HIST_LEN + PEAK_POST + 1:
            return
        head = (self.hist_idx + HIST_RING - HIST_LEN) & (HIST_RING - 1)

        def H(l):
            return self.hist[(head + l) & (HIST_RING - 1)]

        base = HIST_LEN - 1 - PEAK_POST
        oj = H(base)
        if oj <= f(0.0):
            return
        seg_max = oj
        for k in range(base - PEAK_PRE, base + PEAK_POST + 1):
            if H(k) > seg_max:
                seg_max = H(k)
        if oj < seg_max:
            return
        s = f(0.0)
        for k in range(base - PEAK_MEAN_HALF, base + 1):
            s = f(s + H(k))
        mean = f(s / f(PEAK_MEAN_HALF + 1))
        if oj < f(f(f(1.0) + PEAK_DELTA) * mean):
            return
        on = f(oj / self.odf_max) if self.odf_max > f(0.0) else f(0.0)
        if on < PEAK_FLOOR:
            return
        j = int(frame) - PEAK_POST
        gap = s32((int(j) - int(self.prev_peak_frame)) & 0xFFFFFFFF)
        if gap < PEAK_MIN_GAP:
            if on > self.prev_peak_str:
                self.prev_peak_frame = j
                self.prev_peak_str = on
                last = (self.peak_head + PEAK_RING - 1) & (PEAK_RING - 1)
                self.peak_frame[last] = j
                self.peak_str[last] = on
            return
        self.prev_peak_frame = j
        self.prev_peak_str = on
        self.peak_frame[self.peak_head] = j
        self.peak_str[self.peak_head] = on
        self.peak_head = (self.peak_head + 1) & (PEAK_RING - 1)
        if self.peak_count < PEAK_RING:
            self.peak_count += 1

    # ---- the PLL (arTempoAdopt) -------------------------------------------
    def adopt(self, bpm, phase, now):
        P = f(f(f(60.0) * RATE) / f(bpm))
        if not self.locked:
            self.period = P
            self.next_beat = f(phase + f(round(f((now - phase) / P)) * P))
            self.locked = True
            self.beat_index = 0
        else:
            self.period = f(self.period + f(PLL_GAIN * (P - self.period)))
            k = f(round(f((self.next_beat - phase) / self.period)))
            anchor = f(phase + f(k * self.period))
            self.next_beat = f(self.next_beat + f(PLL_GAMMA * (anchor - self.next_beat)))
        self.bpm = f(f(f(60.0) * RATE) / self.period)

    # ---- the sweep --------------------------------------------------------
    def sweep_start(self):
        cutoff = int(self.frame_index) - COMB_FRAMES
        frames, strs = [], []
        total = f(0.0)
        i = 0
        while i < self.peak_count and len(frames) < PEAK_RING:
            idx = (self.peak_head + PEAK_RING - 1 - i) & (PEAK_RING - 1)
            if self.peak_frame[idx] < cutoff:
                break
            frames.append(self.peak_frame[idx])
            strs.append(self.peak_str[idx])
            total = f(total + self.peak_str[idx])
            i += 1

        first, count = 0, CAND_COUNT
        narrow = False
        if self.locked and not self.need_full and self.bpm > f(0.0):
            first, count = self.narrow_window(self.bpm)
            narrow = True
        self.need_full = False
        self.sweep_was_narrow = narrow

        self.sweep_has_narrow = False
        self.sweep_narrow_first = 0
        self.sweep_narrow_count = 0
        self.sweep_narrow_score = f(-1e9)
        self.sweep_narrow_value = f(-1e9)
        self.sweep_narrow_bpm = f(0.0)
        self.sweep_narrow_phase = f(0.0)
        self.sweep_narrow_idx = 0
        self.sweep_far_score = f(-1e9)
        self.sweep_far_value = f(-1e9)
        self.sweep_far_bpm = f(0.0)
        self.sweep_far_phase = f(0.0)
        self.sweep_far_idx = 0
        if (not narrow) and self.locked and self.bpm > f(0.0):
            fi, c = self.narrow_window(self.bpm)
            self.sweep_narrow_first = fi
            self.sweep_narrow_count = c
            self.sweep_has_narrow = True

        self.sweep_frames = frames
        self.sweep_strs = strs
        self.sweep_total = total
        self.sweep_index = 0
        self.sweep_first = first
        self.sweep_n = count
        self.sweep_per_frame = CAND_PER_FRAME if count > CAND_PER_FRAME else count
        self.last_sweep_frame = self.frame_index
        if not narrow:
            self.last_full_frame = self.frame_index
        self.sweep_now = f(self.frame_index)
        self.sweep_best_value = f(-1e9)
        self.sweep_best_score = f(0.0)
        self.sweep_best_bpm = f(0.0)
        self.sweep_best_phase = f(0.0)
        self.sweep_best_idx = 0
        self.sweep_active = len(frames) >= MIN_PEAKS

    def sweep_step(self):
        if not self.sweep_active:
            return
        first = self.sweep_first
        total = self.sweep_n
        done = 0
        while self.sweep_index < total and done < self.sweep_per_frame:
            idx = first + self.sweep_index
            self.sweep_index += 1
            done += 1
            bpm = f(BPM_LO + f(f(idx) * f(CAND_STEP)))
            score, phase = self.comb_score(bpm)
            value = f(score - PRIOR[idx])
            if value > self.sweep_best_value:
                self.sweep_best_value = value
                self.sweep_best_score = score
                self.sweep_best_bpm = bpm
                self.sweep_best_phase = phase
                self.sweep_best_idx = idx
            in_window = (self.sweep_has_narrow
                         and idx >= self.sweep_narrow_first
                         and idx < self.sweep_narrow_first + self.sweep_narrow_count)
            if in_window:
                if score > self.sweep_narrow_score:
                    self.sweep_narrow_score = score
                    self.sweep_narrow_value = value
                    self.sweep_narrow_bpm = bpm
                    self.sweep_narrow_phase = phase
                    self.sweep_narrow_idx = idx
            elif self.sweep_has_narrow:
                if score > self.sweep_far_score:
                    self.sweep_far_score = score
                    self.sweep_far_value = value
                    self.sweep_far_bpm = bpm
                    self.sweep_far_phase = phase
                    self.sweep_far_idx = idx
        if self.sweep_index < total:
            return
        if self.sweep_has_narrow:
            local_alive = self.sweep_narrow_score >= MIN_SCORE
            far_alive = self.sweep_far_score >= MIN_SCORE
            if not local_alive:
                take_far = far_alive
            else:
                take_far = self.sweep_far_score >= f(self.sweep_narrow_score *
                                                     f(f(1.0) + NET_MARGIN))
            if take_far:
                self.sweep_best_value = self.sweep_far_value
                self.sweep_best_score = self.sweep_far_score
                self.sweep_best_bpm = self.sweep_far_bpm
                self.sweep_best_phase = self.sweep_far_phase
                self.sweep_best_idx = self.sweep_far_idx
            else:
                self.sweep_best_value = self.sweep_narrow_value
                self.sweep_best_score = self.sweep_narrow_score
                self.sweep_best_bpm = self.sweep_narrow_bpm
                self.sweep_best_phase = self.sweep_narrow_phase
                self.sweep_best_idx = self.sweep_narrow_idx
        self.score = self.sweep_best_score
        if self.sweep_best_score >= MIN_SCORE:
            self.full_search_failed = False
            self.adopt(self.sweep_best_bpm, self.sweep_best_phase, self.sweep_now)
        elif self.sweep_was_narrow:
            if not self.full_search_failed:
                self.need_full = True
        else:
            self.full_search_failed = True
        self.sweep_active = False

    # ---- the frame (arTempoProcess) ---------------------------------------
    def frame(self, odf):
        self.frame_index += 1
        self.abs_frame += 1
        if odf > self.odf_max:
            self.odf_max = odf
        else:
            self.odf_max = f(self.odf_max * MAX_DECAY)
        self.pick_peak(f(odf), self.frame_index)
        if self.locked:
            while f(self.frame_index) >= self.next_beat:
                self.last_beat = self.next_beat
                self.next_beat = f(self.next_beat + self.period)
                self.beat_index += 1
        gate_open = True
        if gate_open:
            if not self.sweep_active:
                if self.locked and s32(self.frame_index - self.last_full_frame) >= FULL_EVERY:
                    self.need_full = True
                due = (not self.locked or self.need_full
                       or s32(self.frame_index - self.last_sweep_frame) >= SEARCH_EVERY)
                if due:
                    self.sweep_start()
            self.sweep_step()
        # the rebase
        if self.frame_index >= self.rebase and not self.sweep_active:
            self.frame_index -= self.rebase
            self.next_beat = f(self.next_beat - f(self.rebase))
            self.last_beat = f(self.last_beat - f(self.rebase))
            if self.last_sweep_frame >= self.rebase:
                self.last_sweep_frame -= self.rebase
            else:
                self.last_sweep_frame = 0
            if self.last_full_frame >= self.rebase:
                self.last_full_frame -= self.rebase
            else:
                self.last_full_frame = 0
            for i in range(self.peak_count):
                idx = (self.peak_head + PEAK_RING - 1 - i) & (PEAK_RING - 1)
                self.peak_frame[idx] -= self.rebase
            if self.prev_peak_frame >= 0:
                self.prev_peak_frame -= self.rebase
        base = self.abs_frame - self.frame_index
        self.log.append((self.frame_index, float(self.next_beat),
                         float(self.last_beat), float(self.bpm), float(self.score),
                         self.beat_index, self.locked,
                         self.peak_frame[(self.peak_head - 1) & (PEAK_RING - 1)],
                         self.abs_frame,
                         float(self.next_beat) + base,
                         float(self.last_beat) + base))
        return self
        return self


def synth_odf(nframes, period=44.0, amp=1.0, noise=0.02, seed=1):
    rng = np.random.default_rng(seed)
    out = np.zeros(nframes, dtype=np.float32)
    for i in range(nframes):
        ph = (i % period) / period
        if ph < 0.10:
            out[i] = amp * (1.0 - ph / 0.10)
        out[i] += rng.normal(0, noise)
    return np.clip(out, 0, None).astype(np.float32)


def run(rebase, nframes=1500, period=44.0):
    t = Tempo(rebase=rebase)
    odf = synth_odf(nframes, period)
    for i in range(nframes):
        t.frame(odf[i])
    return t


if __name__ == "__main__":
    import sys
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 1500
    rebase = int(sys.argv[2]) if len(sys.argv) > 2 else 512
    a = run(10 ** 9, n)
    b = run(rebase, n)
    print("no rebase : bpm %.4f  score %.4f  locked %s  beats %d"
          % (a.bpm, a.score, a.locked, a.beat_index))
    print("rebase %d : bpm %.4f  score %.4f  locked %s  beats %d"
          % (rebase, b.bpm, b.score, b.locked, b.beat_index))
    # the ABSOLUTE grid must be identical
    worst = 0.0
    worst_i = -1
    for i, (ra, rb) in enumerate(zip(a.log, b.log)):
        d = abs(ra[9] - rb[9])
        if d > worst:
            worst = d
            worst_i = i
    print("worst absolute next_beat difference: %.6f frames at frame %d"
          % (worst, worst_i))
    # the onset frames (absolute) must be identical
    bad = 0
    for i, (ra, rb) in enumerate(zip(a.log, b.log)):
        if ra[7] != rb[7] and abs(ra[7] - rb[7]) != rebase:
            if bad < 10:
                print("  onset frame mismatch at frame %d: %d vs %d"
                      % (i, ra[7], rb[7]))
            bad += 1
    print("onset-frame mismatches (not a whole rebase apart): %d" % bad)
    # the score must not collapse
    worst_s = 1.0
    worst_si = -1
    for i, (ra, rb) in enumerate(zip(a.log, b.log)):
        if rb[4] < worst_s:
            worst_s = rb[4]
            worst_si = i
    print("lowest score with the rebase: %.4f at frame %d" % (worst_s, worst_si))

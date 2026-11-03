#!/usr/bin/env python3
"""REVIEW CHECK, DECISIVE: does the NARROW search walk the grid across a gap?

THE CLAIM
---------
The veto (the 20 % raw-score margin) is applied ONLY to whole-band searches.
The claim is that the narrow search, with the proportional PLL (gain 0.10)
and the window re-centred on the grid every 4 frames, walks the grid ACROSS
the gap on its own:

    grid locked at 125.00, music at 131.00
    narrow window [123,127]; its best is 127 (the edge)
    -> grid moves to 125.20, window becomes [123.2,127.2], and on it goes
    -> 6 BPM crossed in ~30 adopts = ~1.3 s

So the claim makes THREE testable statements:
  (1) the narrow winner sits at the window's EDGE (the score rises
      monotonically toward the truth),
  (2) each adopt moves the grid 0.2 BPM,
  (3) the walk therefore runs at ~4.7 BPM/s and crosses 6 BPM in ~1.3 s.

This script measures all three on a CLEAN 131 BPM beat train, where the
truth is known exactly and the comb's score profile is unambiguous.

    python tools/ar_sim/_rev_walk_synth.py
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _fw_port as P                               # noqa: E402

RATE = P.RATE


def beat_odf(bpm, seconds, noise=0.0, seed=0):
    """An ODF with a clean beat train at `bpm`."""
    n = int(seconds * RATE)
    odf = np.full(n, noise, dtype=np.float64)
    rng = np.random.RandomState(seed)
    if noise:
        odf += rng.uniform(0.0, noise, n)
    period = 60.0 * RATE / bpm
    t = 0.0
    while t < n:
        i = int(round(t))
        if 0 <= i < n:
            odf[i] += 1.0
        t += period
    return odf


class Probe(P.Tracker):
    """Records every adopt: what was adopted, and from which window."""

    def __init__(self):
        super().__init__()
        self.adopts = []          # (frame, adopted bpm, window lo, window hi,
                                  #  winner index in the window)

    def sweep_start(self):
        f, c = P.narrow_window(self.bpm, self.narrow_bpm) \
            if (self.locked and not self.need_full and self.narrow_bpm > 0.0) \
            else (0, 0)
        self._w_lo = P.BPM_LO + f * P.CAND_STEP if c else 0.0
        self._w_hi = P.BPM_LO + (f + c - 1) * P.CAND_STEP if c else 0.0
        self._narrow = bool(c)
        super().sweep_start()

    def sweep_step(self):
        before = self.bpm
        super().sweep_step()
        if self.bpm != before:
            self.adopts.append((self.frame_index, self.bpm, self._w_lo,
                                self._w_hi, self._narrow))


def place_locked(t, bpm, phase=0.0):
    """Put the tracker on a rate, as if it had locked there."""
    t.locked = True
    t.period = 60.0 * RATE / bpm
    t.bpm = bpm
    t.next_beat = phase
    t.last_beat = phase
    t.beat_index = 0
    t.last_sweep_frame = -1000
    t.last_full_frame = -1000


def cfg(narrow=2.0, full_every=32, collapse=True, edge=False, gain=0.10,
        margin=0.20):
    t = Probe()
    t.narrow_bpm = narrow
    t.search_every = 4
    t.edge_rule = edge
    t.collapse_trigger = collapse
    t.full_every = full_every
    t.net_margin = margin
    t.veto_on_collapse = True
    t.pll_gain = gain
    t.exact_kernel = True
    return t


def main():
    truth = 131.0
    odf = beat_odf(truth, 20.0, noise=0.02, seed=1)

    print("=" * 78)
    print("A CLEAN 131 BPM BEAT TRAIN.  The grid is placed at 125.00, locked.")
    print("=" * 78)
    print()

    # ---- 1. the narrow search ALONE -------------------------------------
    t = cfg(full_every=0, collapse=False)
    place_locked(t, 125.0)
    t.run(odf)
    log = np.array(t.bpm_log)

    print("NARROW SEARCH ALONE (no whole-band search at all)")
    nar = [a for a in t.adopts if a[4]]
    print("  adopts %d, of which narrow %d" % (len(t.adopts), len(nar)))
    if nar:
        steps = [nar[i + 1][1] - nar[i][1] for i in range(len(nar) - 1)]
        steps = [s for s in steps if s != 0.0]
        if steps:
            print("  per-adopt step: mean %+.3f BPM   max %+.3f   min %+.3f"
                  % (float(np.mean(steps)), max(steps), min(steps)))
        gaps = [nar[i + 1][0] - nar[i][0] for i in range(len(nar) - 1)]
        if gaps:
            print("  frames between adopts: mean %.1f   min %d   max %d"
                  % (float(np.mean(gaps)), min(gaps), max(gaps)))
        print("  the first 12 narrow adopts:")
        for (f, b, lo, hi, _) in nar[:12]:
            print("    frame %5d (%.2f s)  grid %8.3f  window [%.2f,%.2f]"
                  % (f, f / RATE, b, lo, hi))
        print("  the last 6 narrow adopts:")
        for (f, b, lo, hi, _) in nar[-6:]:
            print("    frame %5d (%.2f s)  grid %8.3f  window [%.2f,%.2f]"
                  % (f, f / RATE, b, lo, hi))
    print("  final rate %.3f  (error %+.3f BPM)  after %.1f s"
          % (log[-1], log[-1] - truth, len(log) / RATE))
    # the walk's speed, measured over the walking part
    if len(nar) > 2:
        f0, b0 = nar[0][0], nar[0][1]
        f1, b1 = nar[-1][0], nar[-1][1]
        print("  WALK SPEED: %+.2f BPM over %.2f s = %.3f BPM/s"
              % (b1 - b0, (f1 - f0) / RATE, (b1 - b0) / ((f1 - f0) / RATE)))
        print("  the claim was 4.7 BPM/s, and 6 BPM in 1.3 s")
    print()

    # ---- 2. the score profile: is the narrow winner at the EDGE? -------
    print("=" * 78)
    print("THE COMB'S SCORE PROFILE ON THE CLEAN TRAIN")
    print("=" * 78)
    frames = np.array([j for j, _ in t.peak_frame], dtype=float)
    strs = np.array([s for _, s in t.peak_frame], dtype=float)
    bps = np.arange(90.0, 175.01, 0.25)
    sc, ph = t.comb_scores_all(frames, strs, bps)
    pen = t.prior
    val = sc - pen
    for lo, hi, tag in ((121.0, 129.0, "the grid's window 123-127"),
                        (125.0, 135.0, "the truth's neighbourhood")):
        sel = (bps >= lo) & (bps <= hi)
        print("  %s:" % tag)
        for b, s, v in zip(bps[sel], sc[sel], val[sel]):
            marks = ""
            if abs(b - truth) < 0.13:
                marks = "   <-- THE TRUTH"
            if 123.0 <= b <= 127.0:
                if abs(b - 123.0) < 0.13 or abs(b - 127.0) < 0.13:
                    marks += "   [WINDOW EDGE]"
                else:
                    marks += "   [in the window]"
            print("    %7.2f  score %6.4f  value %6.4f%s" % (b, s, v, marks))
        print()
    win = (bps >= 123.0) & (bps <= 127.0)
    jb = int(np.argmax(val[win]))
    print("  the narrow winner (best VALUE in [123,127]) is %.2f  (the edge "
          "is 127.00)" % bps[win][jb])
    jb2 = int(np.argmax(sc[win]))
    print("  the best RAW score in [123,127] is %.2f" % bps[win][jb2])
    print()

    # ---- 3. the firmware configuration ----------------------------------
    t2 = cfg()
    place_locked(t2, 125.0)
    t2.run(odf)
    log2 = np.array(t2.bpm_log)
    print("THE FIRMWARE CONFIGURATION (narrow + safety net 32 + veto + "
          "collapse)")
    print("  whole-band searches %d   vetoes %d   far wins %d"
          % (t2.search_full, t2.net_vetoes, t2.net_far))
    for m in (0.25, 0.5, 1.0, 1.3, 2.0, 4.0, 8.0, 16.0):
        f = int(m * RATE)
        if f < len(log2):
            print("    at %5.2f s the grid is %8.3f  (%+.2f from the truth)"
                  % (m, log2[f], log2[f] - truth))
    first = None
    for f in range(len(log2)):
        if abs(log2[f] - truth) <= 0.5:
            first = f
            break
    print("  first within 0.5 BPM of the truth: %s"
          % ("%.2f s" % (first / RATE) if first is not None else "NEVER"))
    print("  final rate %.3f  (error %+.3f BPM)" % (log2[-1], log2[-1] - truth))
    print()

    # ---- 4. how far does the walk get in 1.3 s? -------------------------
    print("=" * 78)
    print("THE CLAIM'S ARITHMETIC, CHECKED")
    print("=" * 78)
    if len(nar) > 2:
        f13 = int(1.3 * RATE)
        before = [a for a in nar if a[0] <= f13]
        if before:
            print("  at 1.3 s the grid is %.3f  (%+.2f BPM) after %d narrow "
                  "adopts"
                  % (before[-1][1], before[-1][1] - truth, len(before)))
        print("  the claim: 6 BPM crossed in 1.3 s (125.00 -> 131.00)")
    print()


if __name__ == "__main__":
    sys.exit(main())

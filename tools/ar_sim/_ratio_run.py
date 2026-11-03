#!/usr/bin/env python3
"""THE MARGIN'S GAP, UNDER THE SHIPPED SEMANTICS.

ar_tempo.h's comment puts three measured bands on the ratio

    far raw score / local raw score

    two rates that are really the same music       1.00 .. 1.13
    two rates where the far one really is better   1.26 .. 1.50
    the music changed and the window turned over   1.23 .. 1.34

and says 0.20 sits in the gap.  Those bands were measured with an OLDER
narrow window (BPM-anchored) and an OLDER narrow winner (by value).  The
shipped veto uses the GRID-anchored window and the RAW-score narrow winner,
and the PLL is proportional now.

This measures the ratio with the SHIPPED definitions, on the SHIPPED
trajectory (the veto in place, gain 0.10):

  UNCHANGED   the music did not change: the far rate is a comb artefact.
              Every one of these has to stay BELOW 1.20, or the veto
              fails to stop the wander.

  CHANGED     the music changed: the far rate is the new music's rate.
              These have to CROSS 1.20, or the tracker never re-locks.

    python tools/ar_sim/_fw_ratio.py --windows 24 --pairs 10
"""

import argparse
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _port_pinned_run as P                               # noqa: E402

NARROW = 2.0
EVERY = 4
NET = 32
MARGIN = 0.20
GAIN = 0.10


def make(margin=MARGIN):
    t = P.Tracker()
    t.narrow_bpm = NARROW
    t.search_every = EVERY
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = NET
    t.net_margin = margin
    t.veto_on_collapse = True
    t.pll_gain = GAIN
    return t


class Probe(P.Tracker):
    """Records the veto's ratio, with the shipped definitions."""

    def __init__(self, defs="new", settle=0):
        super().__init__()
        self.defs = defs
        self.settle = settle      # frames to ignore after the lock
        self.events = []          # (frame, reason, far bpm, narrow bpm, ratio)
        self.settled = []         # ... and the same, once settled
        self._last = None
        self._lock_frame = 0
        self._was_locked = False

    def comb_scores_all(self, frames, strs, bpms):
        scores, phases = super().comb_scores_all(frames, strs, bpms)
        self._last = (np.array(bpms), np.array(scores))
        return scores, phases

    def sweep_start(self):
        reason = self.need_full_reason
        if self.locked != self._was_locked:
            self._lock_frame = self.frame_index
            self._was_locked = self.locked
        self._last = None
        super().sweep_start()
        if self._last is None:
            return
        bpms, scores = self._last
        if len(bpms) != self.cand_count:
            return                      # a narrow search
        if not self.locked or self.bpm <= 0.0:
            return
        f, c = P.narrow_window(self.bpm, self.narrow_bpm)
        idx = np.arange(f, f + c)
        values = scores - self.prior
        if self.defs == "old":
            # the OLD definitions: a BPM-anchored window and a narrow
            # winner by VALUE.  This is what the comment's table was
            # measured with.
            lo = max(P.BPM_LO, self.bpm - self.narrow_bpm)
            hi = min(P.BPM_HI, self.bpm + self.narrow_bpm)
            mask = (bpms >= lo - 1e-6) & (bpms <= hi + 1e-6)
            idx = np.where(mask)[0]
            if not len(idx):
                return
            f = int(idx[0])
            c = len(idx)
            jn = int(idx[np.argmax(values[idx])])
        else:
            # the shipped definitions: the grid-anchored window and a
            # narrow winner by RAW SCORE
            jn = int(idx[np.argmax(scores[idx])])
        # the far winner: by VALUE (score - prior), the whole band
        jg = int(np.argmax(values))
        if f <= jg <= f + c - 1:
            return                      # the far rate IS the local rate
        ratio = float(scores[jg]) / float(scores[jn]) if scores[jn] > 0 else 0.0
        ev = (self.frame_index, reason,
              float(bpms[jg]), float(bpms[jn]), ratio)
        self.events.append(ev)
        if self.frame_index - self._lock_frame >= self.settle:
            self.settled.append(ev)


def pct(a, p):
    return float(np.percentile(np.array(a), p)) if len(a) else float("nan")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=24)
    ap.add_argument("--pairs", type=int, default=10)
    ap.add_argument("--defs", default="new", choices=["new", "old", "both"])
    ap.add_argument("--kernel", default="exact", choices=["exact", "rounded"])
    args = ap.parse_args()

    t0 = time.time()
    items = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, 80.0, 320.0)
        if len(odf) < 1200:
            continue
        items.append((k, odf))
    print("%d windows   %.0f s" % (len(items), time.time() - t0), flush=True)

    # ---- UNCHANGED ----------------------------------------------------
    for defs in (["new", "old"] if args.defs == "both" else [args.defs]):
        ratios = []
        over = []
        settled = []
        for (k, odf) in items:
            t = Probe(defs, settle=469)
            t.narrow_bpm = NARROW
            t.search_every = EVERY
            t.edge_rule = False
            t.collapse_trigger = True
            t.full_every = NET
            t.net_margin = MARGIN
            t.veto_on_collapse = True
            t.pll_gain = GAIN
            t.exact_kernel = (args.kernel == "exact")
            t.run(odf)
            for e in t.events:
                ratios.append(e[4])
                if e[4] >= 1.0 + MARGIN:
                    over.append((k, e))
            for e in t.settled:
                settled.append(e[4])
        print()
        print("UNCHANGED [%s defs]: %d far-outside whole-band searches"
              % (defs, len(ratios)))
        if ratios:
            a = np.array(ratios)
            print("  ratio   min %.3f  p50 %.3f  p90 %.3f  p99 %.3f  max %.3f"
                  % (a.min(), np.median(a), pct(a, 90), pct(a, 99), a.max()))
            print("  above 1.20 (the veto would NOT refuse): %d of %d  (%.1f%%)"
                  % (len(over), len(ratios), 100.0 * len(over) / len(ratios)))
            for (k, e) in over[:10]:
                print("      %-34s frame %6d far %6.2f narrow %6.2f ratio %.3f"
                      % (k[:34], e[0], e[2], e[3], e[4]))
        if settled:
            a = np.array(settled)
            print("  SETTLED (5 s after the lock): %d events  ratio max %.3f"
                  "  p90 %.3f  above 1.20: %d (%.1f%%)"
                  % (len(settled), a.max(), pct(a, 90),
                     int(np.sum(a >= 1.0 + MARGIN)),
                     100.0 * np.sum(a >= 1.0 + MARGIN) / len(settled)))

    # ---- CHANGED ------------------------------------------------------
    print()
    print("CHANGED: the ratio after the cut")
    for i in range(len(items)):
        for j in range(i + 1, len(items)):
            odf_a, odf_b = items[i][1], items[j][1]
            t = Probe()
            t.narrow_bpm = NARROW
            t.search_every = EVERY
            t.edge_rule = False
            t.collapse_trigger = True
            t.full_every = NET
            t.net_margin = MARGIN
            t.veto_on_collapse = True
            t.pll_gain = GAIN
            cut = len(odf_a)
            t.run(np.concatenate((odf_a, odf_b)))
            ev = [e for e in t.events if e[0] >= cut]
            if not ev:
                break
            first = None
            for e in ev:
                if e[4] >= 1.0 + MARGIN:
                    first = e
                    break
            print("  %-28s -> %-28s  %d far events, first over 1.20 at "
                  "%+.1f s (ratio %.3f)"
                  % (items[i][0][:28], items[j][0][:28], len(ev),
                     ((first[0] - cut) / P.RATE) if first else float("nan"),
                     first[4] if first else float("nan")))
            print("      ratios: " + " ".join("%.2f" % e[4] for e in ev[:16]))
            break
        if i >= 5:
            break

    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

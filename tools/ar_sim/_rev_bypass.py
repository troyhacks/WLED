#!/usr/bin/env python3
"""REVIEW CHECK 5, THE BYPASS, MEASURED HONESTLY.

The reviewer's claim, in one sentence: the veto's 20 % evidence rule is
applied ONLY to whole-band searches, so the narrow search can walk the grid
a long way without ever facing the rule.

To test that, separate two kinds of travel:

  APPROVED travel  - the grid moved because a whole-band search TOOK the far
                     (the veto was satisfied).  This is the veto working.
  REFUSED travel   - the grid moved while every whole-band decision in that
                     interval was a REFUSAL.  This is the veto being
                     bypassed, if the movement is large.

So: walk the frame log, mark the frames where a whole-band decision was
taken (approved) and the frames where one was refused, and measure how far
the grid moved inside each REFUSED interval.

Then apply the reviewer's FIX properly - the grid may not drift more than
AR_TEMPO_NARROW_BPM from the last APPROVED rate - and see whether it
changes any outcome.  (The earlier probe was inert: it never recorded the
approved rate, so it blocked nothing.)

    python tools/ar_sim/_rev_bypass.py --pairs 12
"""

import argparse
import os
import sys
import time

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


class Probe(P.Tracker):
    """Records the veto's decisions and every adopt, with its origin."""

    def __init__(self, drift_cap=None):
        super().__init__()
        self.adopts = []           # (frame, bpm, was_narrow)
        self.approved = None       # the last rate the veto APPROVED
        self.drift_cap = drift_cap
        self.blocked = 0

    def sweep_start(self):
        self._narrow = (self.locked and not self.need_full
                        and self.narrow_bpm > 0.0)
        super().sweep_start()

    def sweep_step(self):
        before = self.bpm
        super().sweep_step()
        if self.bpm != before:
            self.adopts.append((self.frame_index, self.bpm, self._narrow))

    def adopt(self, bpm, phase, now, hard=False):
        if self.drift_cap is not None and self.locked and self.approved is not None:
            if abs(bpm - self.approved) > self.drift_cap:
                self.blocked += 1
                bpm = self.approved
        super().adopt(bpm, phase, now, hard=hard)
        if self.sweep_took_far and self.sweep_far_bpm > 0.0:
            self.approved = self.sweep_far_bpm


def firmware_tracker(drift_cap=None):
    t = Probe(drift_cap=drift_cap)
    t.narrow_bpm = NARROW
    t.search_every = EVERY
    t.edge_rule = False            # the C++ has no edge rule
    t.collapse_trigger = True      # the C++ has the collapse path
    t.full_every = NET
    t.net_margin = MARGIN
    t.veto_on_collapse = True
    t.pll_gain = GAIN
    t.exact_kernel = True
    return t


def reference_bpm(odf):
    t = P.Tracker()
    t.narrow_bpm = 0.0
    t.run(odf)
    return t.bpm if t.locked and t.bpm > 0 else None


def load_odfs(windows):
    cache_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "_rev_odf_cache")
    os.makedirs(cache_dir, exist_ok=True)
    items = []
    for k in _cache.keys()[:windows]:
        x = _cache.load(k)
        if x is None:
            continue
        cpath = os.path.join(cache_dir, k.replace("/", "_") + ".npy")
        if os.path.exists(cpath):
            odf = np.load(cpath)
        else:
            odf = B.firmware_odf(x, 80.0, 320.0)
            np.save(cpath, odf)
        if len(odf) < 1200:
            continue
        items.append((k, odf))
    return items


def travel_analysis(t, cut, log):
    """Split the travel into approved and refused stretches."""
    dec = [r for r in t.trace if r["frame"] >= cut]
    # the whole-band decisions, in order
    wb = [r for r in dec if r["reason"] is not None or True]
    # the grid's position at each decision
    intervals = []
    # build the segments between consecutive whole-band decisions
    frames = [r["frame"] for r in dec]
    for i, r in enumerate(dec):
        nxt = frames[i + 1] if i + 1 < len(frames) else len(log)
        lo = max(cut, r["frame"])
        hi = min(nxt, len(log))
        if hi <= lo:
            continue
        seg = log[lo:hi]
        move = float(np.max(np.abs(seg - seg[0])))
        intervals.append((r, move, (hi - lo) / P.RATE, r["took_far"]))
    return intervals


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=30)
    ap.add_argument("--pairs", type=int, default=12)
    ap.add_argument("--gap", type=float, default=8.0)
    args = ap.parse_args()

    t0 = time.time()
    items = load_odfs(args.windows)
    refs = {}
    for (k, odf) in items:
        refs[k] = reference_bpm(odf)
    pairs = []
    for i in range(len(items)):
        a = refs.get(items[i][0])
        if a is None:
            continue
        for j in range(len(items)):
            if j == i:
                continue
            b = refs.get(items[j][0])
            if b is None or abs(b - a) < args.gap:
                continue
            pairs.append((items[i], items[j], b))
            break
    pairs = pairs[:args.pairs]
    print("%d pairs   %.0f s" % (len(pairs), time.time() - t0), flush=True)
    print()

    print("=" * 100)
    print("HOW FAR DID THE GRID MOVE INSIDE A REFUSED STRETCH?")
    print("A stretch is the time between two whole-band searches.  If the")
    print("search at the end of the stretch was REFUSED, the travel inside")
    print("the stretch is travel the veto never sanctioned.")
    print("=" * 100)
    print("%-42s %8s %8s %9s %9s %9s %9s"
          % ("pair", "grid", "B", "max REF", "max OK", "cross", "reached"))
    worst_refused = 0.0
    worst_pair = ""
    reached = 0
    for (a, b, ref) in pairs:
        t = firmware_tracker()
        cut = len(a[1])
        t.run(np.concatenate((a[1], b[1])))
        log = np.array(t.bpm_log)
        iv = travel_analysis(t, cut, log)
        ref_moves = [m for (r, m, d, tk) in iv if not tk]
        ok_moves = [m for (r, m, d, tk) in iv if tk]
        mr = max(ref_moves) if ref_moves else 0.0
        mo = max(ok_moves) if ok_moves else 0.0
        if mr > worst_refused:
            worst_refused = mr
            worst_pair = "%s -> %s" % (a[0][:17], b[0][:17])
        hit = None
        for f in range(cut, len(log)):
            if abs(log[f] - ref) <= 1.0:
                hit = (f - cut) / P.RATE
                break
        reached += 1 if hit is not None else 0
        print("%-42s %8.2f %8.2f %9.2f %9.2f %9s %9s"
              % (("%s -> %s" % (a[0][:17], b[0][:17])),
                 log[cut - 1] if cut else 0.0, ref, mr, mo,
                 ("%.1f s" % hit) if hit is not None else "NO", "-"),
              flush=True)

    print()
    print("the biggest travel inside a REFUSED stretch: %.2f BPM  (%s)"
          % (worst_refused, worst_pair))
    print("the grid reached B's rate in %d of %d pairs" % (reached, len(pairs)))
    print()

    # ---- the reviewer's fix, applied PROPERLY ----------------------------
    print("=" * 100)
    print("WITH THE REVIEWER'S FIX: the grid may not drift more than")
    print("AR_TEMPO_NARROW_BPM (%.1f) from the last APPROVED rate" % NARROW)
    print("=" * 100)
    print("%-42s %8s %8s %9s %9s %9s"
          % ("pair", "grid", "B", "blocked", "reached", "final"))
    reached2 = 0
    blocked_total = 0
    for (a, b, ref) in pairs:
        t = firmware_tracker(drift_cap=NARROW)
        cut = len(a[1])
        t.run(np.concatenate((a[1], b[1])))
        log = np.array(t.bpm_log)
        hit = None
        for f in range(cut, len(log)):
            if abs(log[f] - ref) <= 1.0:
                hit = (f - cut) / P.RATE
                break
        reached2 += 1 if hit is not None else 0
        blocked_total += t.blocked
        print("%-42s %8.2f %8.2f %9d %9s %9.2f"
              % (("%s -> %s" % (a[0][:17], b[0][:17])),
                 log[cut - 1] if cut else 0.0, ref, t.blocked,
                 ("%.1f s" % hit) if hit is not None else "NO", log[-1]),
              flush=True)
    print()
    print("reached B's rate in %d of %d pairs with the fix" % (reached2, len(pairs)))
    print("the fix blocked %d adopts in total" % blocked_total)
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

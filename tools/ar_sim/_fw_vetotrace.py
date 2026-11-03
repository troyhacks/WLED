#!/usr/bin/env python3
"""TRACE THE VETO THROUGH A TRACK CHANGE.

Runs the port's tracker over a pair of windows and prints, for every
WHOLE-BAND search, what the far winner was, what the narrow winner was,
their raw scores, and whether the veto fired.
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402


def make(margin, voc=True):
    t = P.Tracker()
    t.narrow_bpm = 2.0
    t.search_every = 4
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = 32
    t.net_margin = margin
    t.veto_on_collapse = voc
    return t


def reference_bpm(odf):
    t = P.Tracker()
    t.narrow_bpm = 0.0
    t.run(odf)
    return t.bpm if t.locked and t.bpm > 0 else None


def relock_time(t, cut, ref, tol=1.0, hold=281):
    log = t.bpm_log
    n = len(log)
    first = None
    for f in range(cut, n):
        if t.locked and abs(log[f] - ref) <= tol:
            first = f
            break
    if first is None:
        return None
    for f in range(first, min(n, first + hold)):
        if not t.locked or abs(log[f] - ref) > 2.0 * tol:
            return None
    return (first - cut) / P.RATE


class Traced(P.Tracker):
    def __init__(self):
        super().__init__()
        self.events = []
        self._bpm_at_start = 0.0
        self._vetoes = 0

    def sweep_start(self):
        self._bpm_at_start = self.bpm
        self._vetoes = self.net_vetoes
        super().sweep_start()

    def sweep_step(self):
        was_narrow = self.sweep_was_narrow
        reason = self.sweep_reason
        super().sweep_step()
        if was_narrow:
            return
        self.events.append(dict(
            frame=self.frame_index,
            reason=reason,
            grid_bpm=self._bpm_at_start,
            far_bpm=self.sweep_best_bpm,
            far_score=self.sweep_best_score,
            nar_bpm=self.sweep_narrow_bpm,
            nar_score=self.sweep_narrow_score,
            vetoed=(self.net_vetoes > self._vetoes),
            best_score=self.sweep_best_score,
            locked=self.locked,
        ))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=20)
    ap.add_argument("--pairs", type=int, default=8)
    ap.add_argument("--margin", type=float, default=0.20)
    ap.add_argument("--show", type=int, default=3)
    args = ap.parse_args()

    items = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, 80.0, 320.0)
        if len(odf) < 1200:
            continue
        items.append((k, odf))
    print("%d windows" % len(items))

    pairs = []
    for i in range(len(items)):
        bpm = reference_bpm(items[i][1])
        if bpm is None:
            continue
        for j in range(i + 1, len(items)):
            bpm2 = reference_bpm(items[j][1])
            if bpm2 is None or abs(bpm2 - bpm) < 6.0:
                continue
            pairs.append((items[i], items[j], bpm2))
            break
        if len(pairs) >= args.pairs:
            break
    print("%d pairs" % len(pairs))

    shown = 0
    for (a, b, ref) in pairs:
        t = Traced()
        t.narrow_bpm = 2.0
        t.search_every = 4
        t.edge_rule = False
        t.collapse_trigger = True
        t.full_every = 32
        t.net_margin = args.margin
        cut = len(a[1])
        t.run(np.concatenate((a[1], b[1])))
        r = relock_time(t, cut, ref)
        print("\nPAIR %s -> %s   ref=%.2f   relock=%s   bpm_at_end=%.2f"
              % (a[0][:34], b[0][:34], ref,
                 ("%.2f s" % r) if r is not None else "NEVER",
                 t.bpm))
        if r is None and shown < args.show:
            shown += 1
            print("  %-7s %-9s %8s %8s %8s %8s %7s"
                  % ("frame", "reason", "grid", "far", "narrow", "veto?",
                     "far/nar"))
            for e in t.events:
                if e["frame"] < cut - 200:
                    continue
                ratio = (e["far_score"] / e["nar_score"]) if e["nar_score"] > 0 else 0
                print("  %-7d %-9s %8.2f %8.2f %8.2f %8s %7.2f"
                      % (e["frame"], str(e["reason"]), e["grid_bpm"],
                         e["far_bpm"], e["nar_bpm"], e["vetoed"], ratio))
            log = np.array(t.bpm_log)
            print("  bpm after the cut:")
            for i in range(cut, len(log), 60):
                print("    %6d  %s" % (i, " ".join("%6.2f" % v for v in log[i:i + 60])))


if __name__ == "__main__":
    sys.exit(main())

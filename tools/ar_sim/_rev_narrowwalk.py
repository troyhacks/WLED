#!/usr/bin/env python3
"""REVIEW CHECK: can the NARROW search walk the grid a long way?

The claim under test:
  - the veto (the 20 % raw-score margin) is applied ONLY to whole-band
    searches (sweepHasNarrow is set only for !narrow && locked),
  - but the narrow search, with the proportional PLL (gain 0.10) and the
    window re-centred on the grid every 4 frames, can walk the grid across
    a gap with no evidence test at all.

So: run the firmware configuration over cut pairs and watch the grid.

Reports, per pair:
  - the reference rate of B (a whole-band tracker given B alone)
  - the grid's trajectory after the cut
  - whether the grid CROSSED to B's rate (within 1 BPM) and how long it took
  - how many far whole-band winners were REFUSED by the veto
  - whether the crossing happened while the veto was refusing

    python tools/ar_sim/_rev_narrowwalk.py --windows 24 --pairs 12
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


def firmware_tracker():
    """The port, configured the way ar_tempo.h is configured."""
    t = P.Tracker()
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


class Probe(P.Tracker):
    """Logs the veto and the grid."""

    def __init__(self):
        super().__init__()
        self.veto_frames = []
        self.far_ok_frames = []

    def sweep_start(self):
        self._pre_far = self.net_far
        self._pre_veto = self.net_vetoes
        super().sweep_start()

    def sweep_step(self):
        super().sweep_step()
        if self.net_vetoes > self._pre_veto:
            self.veto_frames.append(self.frame_index)
        if self.net_far > self._pre_far and self.net_vetoes == self._pre_veto:
            self.far_ok_frames.append(self.frame_index)


def reference_bpm(odf):
    t = P.Tracker()
    t.narrow_bpm = 0.0
    t.run(odf)
    return t.bpm if t.locked and t.bpm > 0 else None


def run_pair(odf_a, odf_b, full_every=NET, collapse=True, edge=False):
    t = Probe()
    t.narrow_bpm = NARROW
    t.search_every = EVERY
    t.edge_rule = edge
    t.collapse_trigger = collapse
    t.full_every = full_every
    t.net_margin = MARGIN
    t.veto_on_collapse = True
    t.pll_gain = GAIN
    t.exact_kernel = True
    cut = len(odf_a)
    t.run(np.concatenate((odf_a, odf_b)))
    return t, cut


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=24)
    ap.add_argument("--pairs", type=int, default=12)
    args = ap.parse_args()

    t0 = time.time()
    items = []
    cache_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "_rev_odf_cache")
    os.makedirs(cache_dir, exist_ok=True)
    for k in _cache.keys()[:args.windows]:
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
    print("%d windows   %.0f s" % (len(items), time.time() - t0), flush=True)

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
    print("%d pairs   %.0f s" % (len(pairs), time.time() - t0), flush=True)
    print()

    # ---- 1. the firmware configuration ----------------------------------
    print("=" * 100)
    print("THE FIRMWARE CONFIGURATION (narrow 2.0, every 4, net 32, margin 0.20, gain 0.10)")
    print("=" * 100)
    print("%-42s %8s %8s %7s %8s %9s %9s"
          % ("pair", "A", "B", "cross", "vetoes", "far ok", "cross s"))
    crossed = 0
    vetoed_cross = 0
    for (a, b, ref) in pairs:
        t, cut = run_pair(a[1], b[1])
        log = np.array(t.bpm_log)
        first = None
        for f in range(cut, len(log)):
            if t.locked and abs(log[f] - ref) <= 1.0:
                first = f
                break
        # was a far winner refused between the cut and the crossing?
        vetoed_before = sum(1 for f in t.veto_frames if first is None or f <= first)
        far_ok_before = sum(1 for f in t.far_ok_frames if first is None or f <= first)
        cs = (first - cut) / P.RATE if first is not None else None
        crossed += 1 if first is not None else 0
        vetoed_cross += 1 if (first is not None and vetoed_before > 0) else 0
        print("%-42s %8.2f %8.2f %7s %8d %9d %9s"
              % (("%s -> %s" % (a[0][:18], b[0][:18])),
                 t.bpm_log[cut - 1] if cut else 0.0, ref,
                 "yes" if first is not None else "NO",
                 len(t.veto_frames), len(t.far_ok_frames),
                 "%.2f" % cs if cs is not None else "-"), flush=True)
    print()
    print("crossed %d of %d   of those, %d crossed WHILE far winners were being refused"
          % (crossed, len(pairs), vetoed_cross))
    print()

    # ---- 2. no whole-band search at all -----------------------------------
    print("=" * 100)
    print("NO WHOLE-BAND SEARCH AT ALL (full_every=0, collapse off)")
    print("=" * 100)
    crossed2 = 0
    for (a, b, ref) in pairs:
        t, cut = run_pair(a[1], b[1], full_every=0, collapse=False)
        log = np.array(t.bpm_log)
        first = None
        for f in range(cut, len(log)):
            if t.locked and abs(log[f] - ref) <= 1.0:
                first = f
                break
        crossed2 += 1 if first is not None else 0
        print("%-42s %8.2f %8s %9s"
              % (("%s -> %s" % (a[0][:18], b[0][:18])), ref,
                 "yes" if first is not None else "NO",
                 "%.2f s" % ((first - cut) / P.RATE) if first is not None else "-"),
              flush=True)
    print()
    print("crossed %d of %d with no whole-band search" % (crossed2, len(pairs)))
    print()

    # ---- 3. how far does the narrow search walk on its own? ---------------
    print("=" * 100)
    print("THE WALK: the grid's own rate after the cut, no whole-band search")
    print("=" * 100)
    for (a, b, ref) in pairs[:4]:
        t, cut = run_pair(a[1], b[1], full_every=0, collapse=False)
        log = np.array(t.bpm_log)
        traj = []
        for f in range(cut, min(len(log), cut + 400), 20):
            traj.append("%.2f" % log[f])
        print("%-42s B=%.2f  %s" % (("%s -> %s" % (a[0][:18], b[0][:18])),
                                    ref, " ".join(traj)))
    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

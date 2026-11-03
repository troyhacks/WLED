#!/usr/bin/env python3
"""REVIEW CHECK 4, MECHANISM: how does the grid reach a rate the veto
refused - by the narrow walk, or by the veto eventually accepting it?

The question decides whether the reviewer's finding survives.  If the grid
reaches a refused far rate because a WHOLE-BAND search later ACCEPTED it,
the veto did its job and the finding is wrong.  If the grid reaches it by
the narrow path - adopting candidates inside a window that is re-centred on
the grid every four frames - then the veto's evidence rule really was
bypassed, and the finding survives.

For each pair this prints, frame by frame around the crossing:
  - every veto decision (near rate/score, far rate/score, ratio, TAKEN or
    REFUSED)
  - every adopt, and WHICH search produced it (narrow or whole band)
  - the grid's rate

    python tools/ar_sim/_rev_mech.py --pairs 6
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

    def __init__(self):
        super().__init__()
        self.adopts = []           # (frame, bpm, was_narrow, winner_bpm)

    def sweep_start(self):
        self._narrow = (self.locked and not self.need_full
                        and self.narrow_bpm > 0.0)
        super().sweep_start()

    def sweep_step(self):
        before = self.bpm
        winner = self.sweep_best_bpm
        super().sweep_step()
        if self.bpm != before:
            self.adopts.append((self.frame_index, self.bpm, self._narrow,
                                winner))

    def sweep_start_veto(self):
        pass


def firmware_tracker():
    t = Probe()
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=30)
    ap.add_argument("--pairs", type=int, default=6)
    ap.add_argument("--gap", type=float, default=8.0)
    ap.add_argument("--detail", action="store_true")
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

    narrow_moved = 0
    whole_moved = 0
    for (a, b, ref) in pairs:
        t = firmware_tracker()
        cut = len(a[1])
        t.run(np.concatenate((a[1], b[1])))
        log = np.array(t.bpm_log)

        # every veto decision after the cut
        decisions = [r for r in t.trace if r["frame"] >= cut]
        # the far rates that were REFUSED
        refused = [r for r in decisions if not r.get("took_far")]

        # the first time the grid is within 1 BPM of B's rate
        hit = None
        for f in range(cut, len(log)):
            if abs(log[f] - ref) <= 1.0:
                hit = f
                break

        # the first time the grid is within 1 BPM of a REFUSED far rate
        unwound = None
        for r in refused:
            fb = r["far_bpm"]
            if fb <= 0:
                continue
            for f in range(max(cut, r["frame"]), len(log)):
                if abs(log[f] - fb) <= 1.0:
                    unwound = (f, fb, r["frame"])
                    break
            if unwound is not None:
                break

        # how did the grid get there?  which adopts took it across?
        how = "-"
        if unwound is not None:
            (uf, ufb, uframe) = unwound
            # the adopts in the window before the arrival
            ads = [x for x in t.adopts if uframe <= x[0] <= uf]
            n_ad = sum(1 for x in ads if x[2])
            w_ad = len(ads) - n_ad
            # the total movement contributed by the narrow adopts
            narrow_move = 0.0
            for x in ads:
                if x[2]:
                    narrow_move += x[1] - (x[1] / (1.0) if False else 0.0)
            how = "%d narrow adopts, %d whole-band adopts" % (n_ad, w_ad)
            if n_ad:
                narrow_moved += 1
            else:
                whole_moved += 1

        print("=" * 100)
        print("%s -> %s" % (a[0][:40], b[0][:40]))
        print("  the grid at the cut %.2f, B's reference rate %.2f"
              % (log[cut - 1], ref))
        print("  veto decisions after the cut: %d   of which REFUSED: %d"
              % (len(decisions), len(refused)))
        print("  reached B's rate: %s"
              % ("frame %d (%.2f s)" % (hit, (hit - cut) / P.RATE)
                 if hit is not None else "NO"))
        print("  reached a REFUSED far rate: %s"
              % ("frame %d (%.2f s), the far rate was %.2f, refused at "
                 "frame %d" % (unwound[0], (unwound[0] - cut) / P.RATE,
                               unwound[1], unwound[2])
                 if unwound is not None else "NO"))
        print("  how: %s" % how)
        if args.detail and unwound is not None:
            uframe = unwound[2]
            print("  the veto decisions around the refusal and the arrival:")
            for r in decisions:
                if uframe - 100 <= r["frame"] <= unwound[0] + 40:
                    print("    frame %6d (%6.2f s)  grid %8.2f  near %7.2f "
                          "%.4f  far %7.2f %.4f  ratio %5.3f  %s"
                          % (r["frame"], (r["frame"] - cut) / P.RATE, r["bpm"],
                             r["local_bpm"], r["local_score"], r["far_bpm"],
                             r["far_score"], r["ratio"],
                             "FAR TAKEN" if r["took_far"] else "REFUSED"))
            print("  the adopts around it:")
            for (f, bpm, n, winner) in t.adopts:
                if uframe - 100 <= f <= unwound[0] + 40:
                    print("    frame %6d (%6.2f s)  grid -> %8.3f  from %s "
                          "winner %.2f"
                          % (f, (f - cut) / P.RATE, bpm,
                             "narrow    " if n else "whole-band", winner))
        print(flush=True)

    print("the grid reached a refused far rate via the NARROW path in "
          "%d pairs, and via a whole-band search in %d"
          % (narrow_moved, whole_moved))
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

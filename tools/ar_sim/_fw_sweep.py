#!/usr/bin/env python3
"""THE CONSTANTS, AGAINST AN INDEPENDENT TRUTH.

Everything so far has been measured with the tracker judging itself: the
narrow search's own score, the whole-band search's own answer.  That is
how a set of constants can be fitted to a tracker the board does not run.

The corpus has an independent reference: librosa's beat tracker, built by
someone else, against different assumptions.  Its BPM is not truth - it
has its own failures - but a tracker that agrees with it is agreeing with
something outside itself.

So this measures, for a grid of configurations:

  err     the median |published rate - librosa's rate| over the second
          half of the window.  THIS IS TROY'S COMPLAINT: the board says
          125.00 and the music is at 130.
  hit     the fraction of librosa's beats that have a tracker beat near
          them, after searching one constant phase offset
  wander  the standard deviation of the published rate
  jumps   how many times the published rate moved by more than half a BPM
  far     how many times the far candidate was allowed to move the grid

    python tools/ar_sim/_fw_sweep.py --windows 60
"""

import argparse
import json
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402

ORACLE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_oracle")


def oracle(key):
    path = os.path.join(ORACLE_DIR, key + ".json")
    if not os.path.exists(path):
        return None
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def make(prior=0.6, margin=0.20, gain=0.10):
    """The shipped configuration, with the three knobs the grid moves."""
    t = P.Tracker()
    t.narrow_bpm = 2.0
    t.search_every = 4
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = 32
    t.net_margin = margin
    t.pll_gain = gain
    t.prior_weight = prior
    t.prior = t._prior_table()
    return t


def rate_err(published, truth):
    """The distance to the truth, allowing the METRICAL relation.

    librosa's beat tracker reports the half-time rate on some house tracks
    - its lowest answer in this corpus is 64.66 BPM - and the double-time
    on others.  Those are the same music one level apart, so the distance
    is to the nearest of the three levels, and the level itself is
    reported separately.
    """
    errs, levels = [], []
    for t in published:
        d = [abs(t - truth), abs(t - 2.0 * truth), abs(t - truth * 0.5)]
        errs.append(min(d))
        levels.append(int(np.argmin(d)))
    return float(np.median(errs)), levels


def stats(published, truth, beats, truth_beats):
    half = published[len(published) // 2:]
    half = half[half > 0]
    if len(half) < 50:
        return None
    err, levels = rate_err(half, truth)
    jumps = int(np.sum(np.abs(np.diff(half)) > 0.5))
    wander = float(np.std(half))
    if len(beats) and len(truth_beats):
        hit = P.hit_with_offset(np.array(beats) / P.RATE,
                                np.array(truth_beats))[0]
    else:
        hit = 0.0
    return err, hit, wander, jumps, levels


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=60)
    ap.add_argument("--lo", type=float, default=80.0)
    ap.add_argument("--hi", type=float, default=320.0)
    ap.add_argument("--priors", type=str, default="0.0,0.15,0.6")
    ap.add_argument("--margins", type=str, default="0.20,0.45,0.70")
    ap.add_argument("--gains", type=str, default="")
    ap.add_argument("--skip-grid", action="store_true")
    args = ap.parse_args()
    priors = [float(v) for v in args.priors.split(",")]
    margins = [float(v) for v in args.margins.split(",")]
    gains = ([float(v) for v in args.gains.split(",")]
             if args.gains else [0.10])

    t0 = time.time()
    items = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        orc = oracle(k)
        if orc is None or not orc.get("beats"):
            continue
        odf = B.firmware_odf(x, args.lo, args.hi)
        if len(odf) < 1200:
            continue
        items.append((k, odf, float(orc["bpm"]),
                      [b for b in orc["beats"] if 0.0 <= b <= 40.0]))
    print("%d windows   %.0f s" % (len(items), time.time() - t0), flush=True)
    truth = np.array([it[2] for it in items])
    print("librosa's rate: median %.2f   min %.2f   max %.2f"
          % (np.median(truth), truth.min(), truth.max()))
    print()

    def run_table(title, combos):
        print(title)
        print("%-6s %-6s %-6s %-6s | %8s %8s %8s %8s %6s %6s"
              % ("prior", "nprior", "margin", "gain", "err med", "err mean",
                 "hit", "wander", "jumps", "level"))
        for (pr, np_, mg, gn) in combos:
            errs, hits, wand, jumps, fars, lvls = [], [], [], [], [], []
            for (k, odf, tb, tbeats) in items:
                t = make(pr, mg, gn)
                t.prior_in_narrow = np_
                t.run(odf)
                st = stats(np.array(t.bpm_log), tb, t.beats, tbeats)
                if st is None:
                    errs.append(99.0)
                    hits.append(0.0)
                    wand.append(0.0)
                    jumps.append(0)
                    lvls.append(-1)
                else:
                    errs.append(st[0])
                    hits.append(st[1])
                    wand.append(st[2])
                    jumps.append(st[3])
                    # the metrical level: 0 = the truth, 1 = double, 2 = half
                    lvls.append(int(np.bincount(np.array(st[4]),
                                               minlength=3).argmax()))
                fars.append(t.net_far)
            e = np.array(errs)
            h = np.array(hits)
            lv = np.array(lvls)
            onlevel = float(np.mean(lv == 0))
            print("%-6.2f %-6s %-6.2f %-6.2f | %8.3f %8.3f %8.4f %8.3f %6.1f %6.2f"
                  % (pr, "yes" if np_ else "no", mg, gn,
                     float(np.median(e)), float(e.mean()),
                     float(np.median(h)), float(np.mean(wand)),
                     float(np.mean(jumps)), onlevel), flush=True)
        print()

    if not args.skip_grid:
        combos = [(pr, np_, mg, gn)
                  for pr in priors for np_ in (True, False)
                  for mg in margins for gn in gains]
        run_table("THE GRID  (prior x narrow-prior x margin)", combos)

    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

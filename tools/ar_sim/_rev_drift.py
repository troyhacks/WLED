#!/usr/bin/env python3
"""REVIEW CHECK 6, THE DECISIVE ONE: the accumulated drift.

A refusal of one whole-band search does NOT freeze the grid: when the veto
refuses, the sweep still adopts the best rate INSIDE the window (the local
best).  And the narrow search adopts a rate inside the window every four
frames.  The window is re-centred on the grid each time, so the drift is not
bounded by the window.

So the veto's HOLD has to be measured over a RUN of refusals, not over one
refusal.  For each corpus pair this prints, for every run of consecutive
refused whole-band searches:

    the run's length in seconds, the grid at the start of the run, the grid
    at the end of the run, and the DRIFT the run produced.

And the answer to the reviewer's question: the biggest drift a refusal run
produced, and how it compares with the gap to the new rate.

    python tools/ar_sim/_rev_drift.py --pairs 12
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
    t = P.Tracker()
    t.narrow_bpm = NARROW
    t.search_every = EVERY
    t.edge_rule = False
    t.collapse_trigger = True
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


def runs_of_refusals(t, cut, log):
    """The grid's drift over each run of consecutive REFUSED searches."""
    dec = [r for r in t.trace if r["frame"] >= cut]
    out = []
    run = []
    for r in dec:
        if r["took_far"]:
            if run:
                out.append(run)
                run = []
            continue
        if run and run[-1]["frame"] + 1 < r["frame"] - 64:
            # a gap in the decisions: the search was not due.  Treat it as
            # the end of the run only if the grid moved a lot there.
            pass
        run.append(r)
    if run:
        out.append(run)
    res = []
    for run in out:
        f0 = max(cut, run[0]["frame"])
        f1 = min(len(log), run[-1]["frame"] + 34)
        if f1 <= f0:
            continue
        seg = log[f0:f1]
        drift = float(seg[-1] - seg[0])
        res.append({
            "f0": f0, "f1": f1,
            "dur": (f1 - f0) / P.RATE,
            "b0": float(seg[0]), "b1": float(seg[-1]),
            "drift": drift,
            "n": len(run),
            "far_bpm": run[-1]["far_bpm"],
            "far_score": run[-1]["far_score"],
            "near_score": run[-1]["local_score"],
        })
    return res


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

    worst = 0.0
    worst_txt = ""
    total_runs = 0
    for (a, b, ref) in pairs:
        t = firmware_tracker()
        cut = len(a[1])
        t.run(np.concatenate((a[1], b[1])))
        log = np.array(t.bpm_log)
        runs = runs_of_refusals(t, cut, log)
        total_runs += len(runs)
        print("=" * 96)
        print("%s -> %s" % (a[0][:40], b[0][:40]))
        print("  the grid at the cut %.2f, the new rate %.2f, the gap %.2f BPM"
              % (log[cut - 1], ref, abs(ref - log[cut - 1])))
        if not runs:
            print("  no refusal runs")
            continue
        print("  %-5s %8s %9s %9s %9s %9s %9s"
              % ("run", "dur s", "grid0", "grid1", "DRIFT", "far", "ratio"))
        for i, r in enumerate(runs):
            ratio = (r["far_score"] / max(r["near_score"], 1e-9))
            print("  %-5d %8.2f %9.2f %9.2f %+9.2f %9.2f %9.3f"
                  % (i, r["dur"], r["b0"], r["b1"], r["drift"], r["far_bpm"],
                     ratio))
            if abs(r["drift"]) > abs(worst):
                worst = r["drift"]
                worst_txt = ("%s -> %s, run %d: %+.2f BPM over %.2f s "
                             "(grid %.2f -> %.2f), the refused far was %.2f"
                             % (a[0][:17], b[0][:17], i, r["drift"], r["dur"],
                                r["b0"], r["b1"], r["far_bpm"]))
        print(flush=True)

    print("=" * 96)
    print("%d refusal runs over %d pairs" % (total_runs, len(pairs)))
    print("the biggest drift a refusal run produced: %s" % worst_txt)
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

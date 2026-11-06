#!/usr/bin/env python3
"""THE WANDER: how much the published rate moves once the grid is locked.

Troy's report, on the deck: the tempo is locked, and then it moves - 128.0
becomes 127.8, then 128.2, for no reason anyone can hear.  And almost every
real tempo is a whole number.

This measures both on the corpus:

  wander   the standard deviation of the published rate, in BPM, once the
           grid is locked - the wobble
  jumps    how many times the rate moved by more than a quarter of a BPM
           after it had settled
  whole    the fraction of the settled rate that sits on a whole BPM, and
           the fraction that sits on a half - the lattice question
  hit      the fraction of the ground truth's kick onsets with a beat near
           them - the accuracy, so a tidier rate cannot buy its stability
           with a worse grid

The LATTICE knob restricts the candidates to multiples of 0.5 BPM, and
`--bonus` penalises the half integers against the whole ones.

    python tools/ar_sim/_tempo_jitter.py --limit 40
    python tools/ar_sim/_tempo_jitter.py --limit 40 --lattice --bonus 0.01
"""

import argparse
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib                                    # noqa: E402

import _cache                                      # noqa: E402
P = importlib.import_module(os.environ.get("FWPORT", "_fw_port"))  # noqa: E402
from _fw_band import firmware_odf                  # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402


def settled_trace(t):
    """The rate after the grid settled: the last 20 s of the window."""
    log = np.array(t.bpm_log)
    if len(log) < 200:
        return log
    return log[-min(len(log), int(20.0 * P.RATE)):]


def measure(t):
    tr = settled_trace(t)
    if len(tr) < 50:
        return None
    d = np.abs(np.diff(tr))
    jumps = int((d > 0.25).sum())
    fr = np.abs(tr - np.round(tr))
    whole = float((fr < 0.06).mean())
    half = float(((fr > 0.44) & (fr < 0.56)).mean())
    return {
        "wander": float(np.std(tr)),
        "jumps": jumps,
        "whole": whole,
        "half": half,
        "bpm": float(np.median(tr)),
    }


def run(limit, lattice, bonus, quiet=False, margin=None):
    keys = _cache.keys()[:limit]
    rows = []
    hits = []
    t0 = time.time()
    for k in keys:
        x = _cache.load(k)
        if x is None:
            continue
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        if len(ons) < 4:
            continue
        odf = firmware_odf(x, P.BAND_LO, P.BAND_HI)
        if len(odf) < 1200:
            continue
        t = P.Tracker()
        t.quant = 1 if lattice else 0
        t.whole_bonus = bonus
        if margin is not None:
            t.net_margin = margin
        t.run(odf)
        m = measure(t)
        if m is None:
            continue
        f, _off = P.hit_with_offset(np.array(t.beats) / P.RATE, ons)
        m["hit"] = f
        m["key"] = k
        rows.append(m)
        hits.append(f)
        if not quiet and len(rows) % 20 == 0:
            print("  %d/%d  (%.0f s)" % (len(rows), len(keys), time.time() - t0),
                  flush=True)
    return rows


def report(rows, label):
    if not rows:
        print("no windows")
        return
    w = np.array([r["wander"] for r in rows])
    j = np.array([r["jumps"] for r in rows])
    wh = np.array([r["whole"] for r in rows])
    hf = np.array([r["half"] for r in rows])
    h = np.array([r["hit"] for r in rows])
    print()
    print("%s   (%d windows)" % (label, len(rows)))
    print("  wander   median %6.3f BPM   p90 %6.3f   max %6.3f"
          % (float(np.median(w)), float(np.percentile(w, 90)), float(w.max())))
    print("  jumps    median %6.1f       total %6d  (>.25 BPM after settling)"
          % (float(np.median(j)), int(j.sum())))
    print("  whole    %5.1f%% of the settled rate sits on a whole BPM"
          % (100.0 * float(wh.mean())))
    print("  half     %5.1f%% sits on a half BPM"
          % (100.0 * float(hf.mean())))
    print("  hit      median %6.3f   >=0.5: %5.1f%%"
          % (float(np.median(h)), 100.0 * float((h >= 0.5).mean())))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=40)
    ap.add_argument("--lattice", action="store_true")
    ap.add_argument("--bonus", type=float, default=0.0)
    ap.add_argument("--margin", type=float, default=None)
    ap.add_argument("--both", action="store_true",
                    help="run the lattice and the plain grid and compare")
    args = ap.parse_args()

    if args.both:
        a = run(args.limit, False, 0.0, quiet=True, margin=args.margin)
        report(a, "the plain grid (0.25 BPM candidates)")
        b = run(args.limit, True, args.bonus, quiet=True, margin=args.margin)
        report(b, "the lattice (0.5 BPM candidates, bonus %.3f)" % args.bonus)
        return 0

    rows = run(args.limit, args.lattice, args.bonus, margin=args.margin)
    report(rows, "the lattice (0.5 BPM candidates, bonus %.3f)" % args.bonus
           if args.lattice else "the plain grid (0.25 BPM candidates)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

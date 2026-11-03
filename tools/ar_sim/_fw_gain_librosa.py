#!/usr/bin/env python3
"""THE GAIN, MEASURED AGAINST SOMEBODY ELSE.

The gain table that chose the PLL's gain was measured with the tracker's own
instruments - its own hit rate against the raw onsets, its own wander, its own
jump count.  That is the wrong way to choose it, because the raw onsets reward
a tracker that FOLLOWS THE COMB: a fast PLL follows the comb's jitter, and
the jitter is where the onsets are.

The rate is not supposed to follow the jitter.  The rate is supposed to be the
music's rate.  So the gain is chosen against librosa - somebody else's beat
tracker - and against nothing else.

    python tools/ar_sim/_fw_gain_librosa.py --windows 120 --band 90:320
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
import _oracle_librosa as O                        # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402


def make(margin=0.70, gain=0.10, prior=0.6, narrow_prior=False):
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
    t.prior_in_narrow = narrow_prior
    return t


def rate_err(published, truth):
    errs = []
    for t in published:
        d = [abs(t - truth), abs(t - 2.0 * truth), abs(t - truth * 0.5)]
        errs.append(min(d))
    return errs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=120)
    ap.add_argument("--band", type=str, default="90:320")
    ap.add_argument("--margin", type=float, default=0.70)
    ap.add_argument("--gains", type=str, default="0.05,0.10,0.20,0.35")
    ap.add_argument("--prior", type=float, default=0.6)
    ap.add_argument("--narrow-prior", type=int, default=0)
    args = ap.parse_args()
    lo, hi = [float(v) for v in args.band.split(":")]
    gains = [float(v) for v in args.gains.split(",")]

    t0 = time.time()
    items = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        orc = O.load(k)
        if orc is None or len(ons) < 4 or len(orc["beats"]) < 4:
            continue
        odf = B.firmware_odf(x, lo, hi)
        if len(odf) < 1200:
            continue
        items.append((k, odf, ons, np.array(orc["beats"], dtype=float),
                      orc["bpm"]))
    print("%d windows   band %g-%g Hz   margin %.2f   %.0f s"
          % (len(items), lo, hi, args.margin, time.time() - t0), flush=True)
    print()
    print("%-7s | %8s %8s %8s | %8s %8s | %8s %8s | %7s %6s"
          % ("gain", "err med", "err mean", "err p90", "beats med",
             "beats mean", "kick med", "kick mean", "wander", "jumps"))
    print("-" * 100)
    for g in gains:
        errs, hits, kicks = [], [], []
        wd, jp = [], []
        for (k, odf, ons, lb, lbpm) in items:
            t = make(args.margin, g, args.prior, bool(args.narrow_prior))
            t.run(odf)
            if not t.beats:
                continue
            tb = np.array(t.beats) / P.RATE
            errs.extend(rate_err([t.bpm], lbpm))
            hits.append(P.hit_with_offset(tb, lb)[0])
            kicks.append(P.hit_with_offset(tb, ons)[0])
            log = np.array(t.bpm_log)
            half = log[len(log) // 2:]
            half = half[half > 0]
            if len(half) > 50:
                wd.append(float(np.std(half)))
                jp.append(int(np.sum(np.abs(np.diff(half)) > 0.5)))
        e = np.array(errs)
        hh = np.array(hits)
        kk = np.array(kicks)
        print("%-7.2f | %8.3f %8.3f %8.3f | %8.4f %8.4f | %8.4f %8.4f | %7.3f %6.1f"
              % (g, float(np.median(e)), float(e.mean()),
                 float(np.percentile(e, 90)),
                 float(np.median(hh)), float(hh.mean()),
                 float(np.median(kk)), float(kk.mean()),
                 float(np.mean(wd)) if wd else 0.0,
                 float(np.mean(jp)) if jp else 0.0), flush=True)
    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

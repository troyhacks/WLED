#!/usr/bin/env python3
"""WHAT IS THE VETO ACTUALLY DECIDING?

The last sweep said the far candidate is adopted ~72 % of the whole-band
sweeps and the rate wanders.  That is either

  (a) the veto is broken - it lets the far win when it should not, or
  (b) the local rate really is losing the comparison, which would mean the
      tracker is NOT locked on the right tempo even without the veto.

(b) and (a) look the same from the outside, so this measures them apart:

  VETO OFF   a margin so large the far can never win.  The tracker is then
             the narrow search plus the PLL, and it says what the truth is.
  VETO ON    the real thing, with every sweep's decision printed.

For each whole-band sweep it prints the LOCAL rate and its score, the FAR
rate and its score, and the truth - the tempo that the ground-truth onsets
actually have, found by autocorrelation of the kick onsets.

    python tools/ar_sim/_fw_vetodiag.py --windows 3
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402


def true_tempo(onsets, rate=P.RATE):
    """The tempo the ground-truth onsets have, by autocorrelation."""
    if len(onsets) < 8:
        return 0.0
    o = np.asarray(onsets, dtype=float)
    # the onset times in seconds
    t = o / rate
    lo, hi = 60.0 / 175.0, 60.0 / 90.0
    lags = np.arange(int(lo * rate), int(hi * rate) + 1)
    best, bestv = 0.0, -1.0
    for lag in lags:
        # count the pairs that are one lag apart
        d = np.abs(o[:, None] - o[None, :] - lag)
        v = float(np.sum(np.exp(-0.5 * (d / 2.0) ** 2)))
        if v > bestv:
            bestv, best = v, lag
    return 60.0 * rate / best if best else 0.0


class Diag(P.Tracker):
    """The tracker, but it says what each sweep decided."""

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.decisions = []

    def sweep_step(self):
        was_active = self.sweep_active
        super().sweep_step()
        if was_active and self.sweep_best_score > -1e8:
            self.decisions.append({
                "bpm": self.bpm,
                "local_bpm": self.sweep_narrow_bpm,
                "local_score": self.sweep_narrow_score,
                "far_bpm": self.sweep_far_bpm,
                "far_score": self.sweep_far_score,
                "took_far": self.net_far > 0 and self.decisions
                and self.decisions[-1] is not self.decisions[-1],
            })


def run(window, lo=80.0, hi=320.0, margin=0.20, gain=0.10):
    x = _cache.load(window)
    odf = B.firmware_odf(x, lo, hi)
    ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
    truth = true_tempo(ons)

    out = {}
    for name, m in (("VETO OFF", 100.0), ("VETO ON", margin)):
        t = P.Tracker()
        t.narrow_bpm = 2.0
        t.search_every = 4
        t.edge_rule = False
        t.collapse_trigger = True
        t.full_every = 32
        t.net_margin = m
        t.pll_gain = gain
        t.run(odf)
        log = np.array(t.bpm_log)
        log = log[log > 0]
        half = log[len(log) // 2:]
        out[name] = {
            "truth": truth,
            "final": float(half[-1]) if len(half) else 0.0,
            "median": float(np.median(half)) if len(half) else 0.0,
            "std": float(np.std(half)) if len(half) else 0.0,
            "jumps": int(np.sum(np.abs(np.diff(half)) > 0.5)) if len(half) > 1 else 0,
            "far": t.net_far,
            "vetoes": t.net_vetoes,
            "score": t.score,
        }
    return out, truth


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=3)
    ap.add_argument("--band-lo", type=float, default=80.0)
    ap.add_argument("--band-hi", type=float, default=320.0)
    ap.add_argument("--margin", type=float, default=0.20)
    args = ap.parse_args()

    n = 0
    for k in _cache.keys()[:args.windows * 3]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, args.band_lo, args.band_hi)
        if len(odf) < 1200:
            continue
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        if len(ons) < 8:
            continue
        out, truth = run(k, args.band_lo, args.band_hi, args.margin)
        print("%s   truth %.2f BPM" % (k[:34], truth))
        for name in ("VETO OFF", "VETO ON"):
            d = out[name]
            print("   %-9s final %7.2f  median %7.2f  std %6.3f  "
                  "jumps %3d  far %3d  veto %3d  score %.3f"
                  % (name, d["final"], d["median"], d["std"], d["jumps"],
                     d["far"], d["vetoes"], d["score"]))
        print()
        n += 1
        if n >= args.windows:
            break


if __name__ == "__main__":
    sys.exit(main())

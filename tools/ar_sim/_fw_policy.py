#!/usr/bin/env python3
"""THE KERNEL'S POLICY - which box the comb scores with.

The rate bias is measured: on real house tracks the raw score's argmax sits
at 169 BPM median while the music is at 128, and on a DJ-style mix the
published rate climbs to the top of the band and stays there.

The cause is the box's WIDTH: it is fixed in TIME, so a fast candidate's
box covers a bigger FRACTION of its period, and it can merge two beat
clusters that a wrong rate has put a third of a period apart.

This measures the policies against each other:

  time   the box is a fixed width in time        (the firmware today)
  frac   the box is a fixed FRACTION of the period
  min    the tolerance, but never over a sixth of the period
  dens   the box's MEAN per bin, not its sum
  exc    the capture above the capture a uniform phase would give

    python tools/ar_sim/_fw_policy.py --windows 119 --mix 12
"""

import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402

ORACLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_oracle")
POLICIES = ["time", "frac", "min", "dens", "exc"]

_KICK = {}


def kick_onsets_for(key):
    """The kick onsets for a window, cached."""
    if key not in _KICK:
        x = _cache.load(key)
        _KICK[key] = (kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
                      if x is not None else None)
    return _KICK[key]


def oracle_bpm(key):
    p = os.path.join(ORACLE, key + ".json")
    if not os.path.exists(p):
        return 0.0
    with open(p, encoding="utf-8") as f:
        return float(json.load(f).get("bpm", 0.0))


def make(policy, frac=0.09, gain=0.20, margin=0.70, min_score=None,
         plateau='lead'):
    t = P.Tracker()
    t.narrow_bpm = 2.0
    t.search_every = 4
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = 32
    t.net_margin = margin
    t.pll_gain = gain
    t.prior_weight = 0.6
    t.prior = t._prior_table()
    t.prior_in_narrow = False
    t.kernel_policy = policy
    t.kernel_frac = frac
    t.plateau = plateau
    if min_score is not None:
        t.min_score = min_score
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=119)
    ap.add_argument("--mix", type=int, default=12)
    ap.add_argument("--fracs", type=str, default="0.09")
    ap.add_argument("--policies", type=str, default=",".join(POLICIES))
    ap.add_argument("--plateau", type=str, default="lead")
    args = ap.parse_args()

    pols = args.policies.split(",")
    fracs = [float(v) for v in args.fracs.split(",")]
    keys = _cache.keys()[:args.windows]
    lib = {}
    items = []
    for k in keys:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, 90.0, 320.0)
        if len(odf) < 1200:
            continue
        items.append((k, odf))
        lib[k] = oracle_bpm(k)
    print("%d windows" % len(items), flush=True)

    combos = []
    for p in pols:
        if p in ('frac', 'minfrac'):
            combos += [(p, f) for f in fracs]
        else:
            combos.append((p, 0.0))

    print()
    print("%-6s %-5s | %8s %8s %8s | %8s %8s | %7s %6s | %7s %7s"
          % ("policy", "frac", "err med", "err mean", "err p90",
             "bpm med", "score med", "wander", "jumps", "off ms", "off frac"))
    for (pol, frac) in combos:
        errs, bpms, scores, wanders, jumps = [], [], [], [], []
        offs, ofracs = [], []
        for (k, odf) in items:
            t = make(pol, frac, min_score=0.0, plateau=args.plateau)
            t.run(odf)
            log = np.array(t.bpm_log)
            log = log[log > 0]
            if len(log) == 0:
                errs.append(999.0)
                bpms.append(0.0)
                scores.append(0.0)
                wanders.append(0.0)
                jumps.append(0)
                continue
            lb = lib.get(k, 0.0)
            if lb > 0:
                e = float(np.median(np.abs(log - lb)))
                errs.append(e)
            bpms.append(float(np.median(log)))
            sc = np.array(t.score_log)
            scores.append(float(np.median(sc)) if len(sc) else 0.0)
            half = log[len(log) // 2:]
            wanders.append(float(np.std(half)))
            jumps.append(int(np.sum(np.abs(np.diff(half)) > 0.5)))
            # the PHASE: where the published beats sit against the kick
            ons = kick_onsets_for(k)
            if ons is not None and len(t.beats):
                f, off = P.hit_with_offset(np.array(t.beats) / P.RATE, ons)
                offs.append(off * 1000.0)
                ofracs.append(off * float(np.median(log)) / 60.0)
        e = np.array(errs)
        oa = np.array(offs) if offs else np.zeros(1)
        fa = np.array(ofracs) if ofracs else np.zeros(1)
        print("%-6s %-5.2f | %8.3f %8.3f %8.3f | %8.2f %8.3f | %7.3f %6.1f | %7.1f %7.3f"
              % (pol, frac, float(np.median(e)), float(e.mean()),
                 float(np.percentile(e, 90)), float(np.median(bpms)),
                 float(np.median(scores)), float(np.mean(wanders)),
                 float(np.mean(jumps)), float(np.median(oa)),
                 float(np.median(fa))), flush=True)

    # ---- the DJ-style mix ------------------------------------------------
    print()
    print("the DJ-style MIX: the rate over time, per policy")
    picks = []
    seen = set()
    for k in _cache.keys()[:400]:
        tr = k.split("@")[0]
        if tr in seen:
            continue
        seen.add(tr)
        picks.append(k)
        if len(picks) >= args.mix:
            break
    mix = []
    for k in picks:
        x = _cache.load(k)
        if x is None:
            continue
        mix.append(np.asarray(x, dtype=np.float64))
        mix.append(np.zeros(int(0.05 * _cache.CACHE_RATE)))
    audio = np.concatenate(mix)
    odf = B.firmware_odf(audio, 90.0, 320.0)
    print("the mix's librosa rates: %s"
          % " ".join("%.0f" % oracle_bpm(k) for k in picks))
    for (pol, frac) in combos:
        t = make(pol, frac, min_score=0.0, plateau=args.plateau)
        t.run(odf)
        log = np.array(t.bpm_log)
        log[log <= 0] = np.nan
        n = len(log)
        step = max(1, n // 18)
        pts = " ".join("%.0f" % log[i] for i in range(0, n, step))
        print("  %-6s %-5.2f  %s" % (pol, frac, pts), flush=True)


if __name__ == "__main__":
    sys.exit(main())

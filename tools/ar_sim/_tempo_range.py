#!/usr/bin/env python3
"""THE RANGE, MEASURED ON THE REAL MUSIC.

WHAT IS MISSING
---------------
The corpus has 300 windows of real house music and a librosa oracle for
every one of them.  Every band experiment so far measured the tracker
against `kick_onsets`, which is itself a 40-160 Hz detector - so the
tracker and the ground truth were looking at the same events, and the
band could win by being self-consistent rather than by being right.

The oracle is better: librosa is an independent implementation (Ellis' DP
over a tempo prior with a comb phase score), it saw the WHOLE band, and
its beats are times.  So the tracker can be scored by whether ITS beats
line up with the oracle's beats - the same test `hit_with_offset` already
uses, but against a truth that did not come from the same filter.

WHAT THIS SETTLES
-----------------
The shipped band is 90-175.  The octave-free condition is
Bmax < 2*Bmin, so 90-175 has a margin of only +2.5 BPM: the 90 floor and
the 175 ceiling are 4.5 BPM apart in ratio terms, and the half-time of a
90 BPM track is 45 - out of band, fine - but the DOUBLE of a 90 BPM track
is 180, just outside the 175 ceiling, so the track is only ONE BPM of
margin from being read at twice its speed.

This measures, on the real music:
  1. the hit rate against the oracle, per band
  2. the octave errors per band (the estimate is twice or half the truth)
  3. what the prior is worth, on its own and inside a band
  4. what each band costs the corpus (how many windows are out of band)

    python tools/ar_sim/_tempo_range.py --limit 300
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

ORACLE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "_oracle")


# --------------------------------------------------------------------------
# The oracle.
# --------------------------------------------------------------------------

def oracle(key):
    """(bpm, beats in seconds) for one window, or None."""
    p = os.path.join(ORACLE_DIR, key + ".json")
    if not os.path.exists(p):
        return None
    with open(p, "r", encoding="utf-8") as f:
        d = json.load(f)
    return float(d["bpm"]), np.asarray(d["beats"], dtype=float)


# --------------------------------------------------------------------------
# One configuration.
# --------------------------------------------------------------------------

def configure(lo, hi, prior_bpm=125.0, prior_sigma=0.35, prior_weight=0.6,
              step=0.25):
    """Set the port's globals, the way the C++'s constants would change."""
    P.BPM_LO = float(lo)
    P.BPM_HI = float(hi)
    P.CAND_STEP = float(step)
    P.CAND_COUNT = int(round((float(hi) - float(lo)) / float(step))) + 1
    P.PRIOR_BPM = float(prior_bpm)
    P.PRIOR_SIGMA = float(prior_sigma)
    P.PRIOR_WEIGHT = float(prior_weight)


def run_one(x, odf, cfg):
    """Run the tracker, return (hit fraction, estimated bpm, offset)."""
    configure(**cfg)
    t = P.Tracker()
    t.run(odf)
    if not t.beats:
        return 0.0, 0.0, 0.0
    b = np.asarray(t.beats, dtype=float) / P.RATE
    return b, t.bpm, t.score


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=300)
    ap.add_argument("--lo", type=float, default=40.0)
    ap.add_argument("--hi", type=float, default=160.0)
    args = ap.parse_args()

    # the bands to try.  Each is (label, lo, hi, prior_weight).
    #
    # The prior is what the tree ships: a log-normal centred at 125 BPM.
    # The digest argues a BAND is the honest mechanism and the prior is a
    # substitute for it, so one band is also run with the prior off.
    bands = [
        ("shipped 90-175 +prior", 90.0, 175.0, 0.6),
        ("shipped 90-175 noprior", 90.0, 175.0, 0.0),
        ("90-176 +prior", 90.0, 176.0, 0.6),
        ("95-176 +prior", 95.0, 176.0, 0.6),
        ("100-176 +prior", 100.0, 176.0, 0.6),
        ("110-176 +prior", 110.0, 176.0, 0.6),
        ("118-176 +prior", 118.0, 176.0, 0.6),
        ("120-176 +prior", 120.0, 176.0, 0.6),
        ("118-176 noprior", 118.0, 176.0, 0.0),
        ("120-176 noprior", 120.0, 176.0, 0.0),
        ("85-170 +prior", 85.0, 170.0, 0.6),
        ("70-140 +prior", 70.0, 140.0, 0.6),
    ]

    keys = _cache.keys()[:args.limit]
    # the truth, once
    truths = []
    for k in keys:
        o = oracle(k)
        if o is None:
            continue
        truths.append((k, o[0], o[1]))
    print("%d windows with an oracle" % len(truths))

    # the ODF, once
    t0 = time.time()
    odfs = {}
    for (k, _, _) in truths:
        x = _cache.load(k)
        if x is None:
            continue
        odfs[k] = B.firmware_odf(x, args.lo, args.hi)
    print("odf: %.0f s" % (time.time() - t0))

    # the truth's own spread
    tb = np.array([t[1] for t in truths])
    print("oracle bpm: min %.1f max %.1f median %.1f" %
          (tb.min(), tb.max(), float(np.median(tb))))
    print()
    print("%-24s %7s %7s %7s %7s %7s %6s" %
          ("band", "hit>=.5", "hit>=.8", "median", "octave", "outband", "cost"))
    for (label, lo, hi, pw) in bands:
        hits = []
        oct_err = 0
        outb = 0
        bpms = []
        for (k, tbpm, beats) in truths:
            odf = odfs.get(k)
            if odf is None or len(beats) < 4:
                continue
            b, est, score = run_one(None, odf, dict(lo=lo, hi=hi,
                                                    prior_weight=pw))
            if not (isinstance(b, np.ndarray) and len(b)):
                hits.append(0.0)
                bpms.append(0.0)
                continue
            f, _off = P.hit_with_offset(b, beats)
            hits.append(f)
            bpms.append(est)
            if est > 0:
                r = est / tbpm
                if abs(r - 2.0) < 0.08 or abs(r - 0.5) < 0.08:
                    oct_err += 1
                if est < lo - 0.5 or est > hi + 0.5:
                    outb += 1
        a = np.array(hits)
        n = len(a)
        # the cost: the number of candidates, and the frames they cost
        ncand = int(round((hi - lo) / 0.25)) + 1
        cost = ncand * (1125.0 / 93.75)      # candidate-passes per second
        print("%-24s %6.1f%% %6.1f%% %7.3f %7d %7d %6d" %
              (label,
               100.0 * float((a >= 0.5).mean()),
               100.0 * float((a >= 0.8).mean()),
               float(np.median(a)),
               oct_err, outb, cost))
    return 0


if __name__ == "__main__":
    sys.exit(main())

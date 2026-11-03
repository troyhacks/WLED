#!/usr/bin/env python3
"""THE DOWNBEAT - is there a beat in the bar that is DIFFERENT from the rest?

Troy's rule: the tempo code emits four beats to the bar, and the first one
is a GUESS.  A better guess is a MEASUREMENT: if one beat in the bar is
regularly different from the other three - louder, a different transient -
then that one is the downbeat.  And if one beat in SIXTEEN is different,
then that is the measure.

The statistic is a t-score: how far the strongest position's mean stands
above the bar's mean, in units of the standard error.  And it is measured
against a NULL - the same beats with the bar's alignment rolled by a random
amount, which is the same statistic on data with no bar structure in it.

A feature whose score sits ON its null carries no bar-level information,
and the tracker must not claim a downbeat from it.

    python tools/ar_sim/_fw_down.py --windows 60
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402


def make():
    t = P.Tracker()
    t.narrow_bpm = 2.0
    t.search_every = 4
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = 32
    t.net_margin = 0.70
    t.pll_gain = 0.20
    t.prior_weight = 0.6
    t.prior = t._prior_table()
    t.prior_in_narrow = False
    t.kernel_policy = 'minfrac'
    t.kernel_frac = 0.14
    t.plateau = 'centre'
    t.min_score = 0.0
    return t


def peak_at(series, beats, half):
    out = []
    n = len(series)
    for b in beats:
        i = int(round(b))
        lo = max(0, i - half)
        hi = min(n, i + half + 1)
        out.append(float(np.max(series[lo:hi])) if hi > lo else 0.0)
    return np.array(out)


def profile(sal, n):
    p = np.zeros(n)
    c = np.zeros(n)
    for i, s in enumerate(sal):
        p[i % n] += s
        c[i % n] += 1
    c[c == 0] = 1
    return p / c


def tscore(sal, n):
    """How far the strongest position stands above the bar's mean, in
    standard errors.  The positions' counts differ by at most one, so the
    standard error is taken over all of them."""
    m = profile(sal, n)
    cnt = np.array([np.sum((np.arange(len(sal)) % n) == p) for p in range(n)])
    cnt[cnt == 0] = 1
    se = float(np.std(sal)) / np.sqrt(cnt.mean())
    if se <= 1e-12:
        return 0.0, 0
    return float((m.max() - m.mean()) / se), int(np.argmax(m))


def null_score(sal, n, rolls=12, seed=1234):
    rng = np.random.default_rng(seed)
    out = []
    for _ in range(rolls):
        r = int(rng.integers(1, n))
        out.append(tscore(np.roll(sal, r), n)[0])
    return float(np.mean(out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=60)
    ap.add_argument("--half-ms", type=float, default=50.0)
    args = ap.parse_args()

    half = int(round((args.half_ms * 0.001) * P.RATE))
    bands = ((90.0, 320.0), (40.0, 160.0))
    feats = ("odf_peak", "level")
    acc = {}

    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, 90.0, 320.0)
        if len(odf) < 1200:
            continue
        t = make()
        t.run(odf)
        if len(t.beats) < 32:
            continue
        b = t.beats
        for (lo, hi) in bands:
            o = odf if (lo, hi) == (90.0, 320.0) else B.firmware_odf(x, lo, hi)
            d = B.firmware_db(x, lo, hi)
            sals = {
                "odf_peak": peak_at(o, b, half),
                "level": peak_at(d - d.mean(), b, half),
            }
            for f, s in sals.items():
                if len(s) < 32:
                    continue
                key = ("%g-%g" % (lo, hi), f)
                rec = acc.setdefault(key, {"t4": [], "n4": [], "t16": [], "n16": [],
                                           "a4": []})
                t4, a4 = tscore(s, 4)
                t16, _ = tscore(s, 16)
                rec["t4"].append(t4)
                rec["n4"].append(null_score(s, 4))
                rec["t16"].append(t16)
                rec["n16"].append(null_score(s, 16))
                rec["a4"].append(a4)

    print("%d windows" % len(acc[(bands[0][0], "odf_peak")]["t4"])
          if (bands[0][0], "odf_peak") in acc else "no windows")
    print()
    print("%-14s %-9s | %7s %7s %7s | %7s %7s | %6s"
          % ("band", "feature", "t4", "null4", "above", "t16", "null16", "the argmax"))
    for key in sorted(acc):
        rec = acc[key]
        t4 = np.array(rec["t4"]); n4 = np.array(rec["n4"])
        t16 = np.array(rec["t16"]); n16 = np.array(rec["n16"])
        print("%-14s %-9s | %7.3f %7.3f %6.0f%% | %7.3f %7.3f | %6d"
              % (key[0], key[1], np.median(t4), np.median(n4),
                 100.0 * (t4 > n4).mean(), np.median(t16), np.median(n16),
                 int(np.bincount(rec["a4"], minlength=4).argmax())))


if __name__ == "__main__":
    sys.exit(main())

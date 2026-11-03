#!/usr/bin/env python3
"""THE DECIDING MEASUREMENT: the prior's (centre, weight), scored the
right way.

THE PROBLEM WITH THE RAW CORPUS NUMBER
---------------------------------------
The club band is 118-176.  The corpus's oracle truths run from 63.92 to
170.45, and about 23 of every 300 windows are BELOW the band.  Those windows
can never be read correctly - the band is a promise the tracker keeps, so
it is not allowed to report 95 BPM - and they are counted as failures in
every corpus score printed so far.  That caps the corpus number and hides
which configuration is actually better on the music the band is for.

So this scores the two populations separately:

  IN   the windows whose truth is inside the band   -> the hit rate that
       matters: of the music the band promises to explain, how much does
       it explain?

  OUT  the windows whose truth is below the band     -> the tracker should
       NOT claim a fit here.  It will (the alias is in-band and the band
       forbids the truth), so the number to watch is how many it claims
       ANYWAY, and how confident it is.

And it sweeps the centre and the weight together.

    python tools/ar_sim/_tempo_decide.py --limit 120
"""

import argparse
import multiprocessing as mp
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402
from _tempo_range import configure, oracle         # noqa: E402

LO, HI = 118.0, 176.0
SIGMA = 0.35
CONFIGS = [
    (0.00, 118.0), (0.00, 125.0),
    (0.20, 118.0), (0.20, 125.0),
    (0.30, 118.0), (0.30, 125.0),
    (0.40, 118.0), (0.40, 125.0),
    (0.60, 118.0), (0.60, 125.0),
]


def cfg(w, c):
    configure(lo=LO, hi=HI, prior_weight=w, prior_bpm=c, prior_sigma=SIGMA)


def one(key):
    o = oracle(key)
    if not o:
        return None
    tb = float(o[0])
    x = _cache.load(key)
    if x is None:
        return None
    odf = B.firmware_odf(x, 80.0, 320.0)
    out = []
    for (w, c) in CONFIGS:
        cfg(w, c)
        t = P.Tracker()
        t.run(odf)
        e = t.bpm
        if e <= 0:
            cls = "none"
        elif abs(e / tb - 1.0) <= 0.03:
            cls = "right"
        elif abs(e / (2.0 * tb) - 1.0) <= 0.03:
            cls = "fast"
        elif abs(e / (0.5 * tb) - 1.0) <= 0.03:
            cls = "half"
        elif abs(e / tb - 4.0 / 3.0) < 0.06:
            cls = "4:3"
        elif abs(e / tb - 1.25) < 0.04:
            cls = "5:4"
        elif abs(e / tb - 1.5) < 0.06:
            cls = "3:2"
        else:
            cls = "other"
        out.append(((w, c), cls, float(e), float(t.score) if hasattr(t, "score") else 0.0))
    return (tb, out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=120)
    args = ap.parse_args()

    rows = []
    with mp.Pool(8) as pool:
        for r in pool.imap(one, _cache.keys()[:args.limit]):
            if r:
                rows.append(r)
    print("%d windows\n" % len(rows))

    truths = np.array([r[0] for r in rows])
    inside = (truths >= LO - 1e-6) & (truths <= HI + 1e-6)
    print("the band %g-%g covers %d of %d windows (%.0f%%); %d are below it\n"
          % (LO, HI, int(inside.sum()), len(rows), 100.0 * inside.mean(),
             int((~inside).sum())))

    print("%-16s %-28s %-28s" % ("config", "IN the band (n=%d)" % int(inside.sum()),
                                 "OUT the band (n=%d)" % int((~inside).sum())))
    print("%-16s %-28s %-28s" % ("", "right  alias  other", "claims  4:3  other"))
    for (w, c) in CONFIGS:
        ins, outs = [], []
        for (tb, out) in rows:
            for (key, cls, e, sc) in out:
                if key != (w, c):
                    continue
                if LO - 1e-6 <= tb <= HI + 1e-6:
                    ins.append(cls)
                else:
                    outs.append(cls)
        i = {k: ins.count(k) for k in set(ins)}
        o = {k: outs.count(k) for k in set(outs)}
        nright = i.get("right", 0)
        print("w=%.2f C=%-6.0f %-28s %-28s"
              % (w, c,
                 "%3d/%3d %3d %3d %3d" % (nright, len(ins), i.get("4:3", 0),
                                         i.get("5:4", 0), i.get("other", 0)),
                 "%3d/%3d %3d %3d %3d" % (len(outs) - o.get("none", 0), len(outs),
                                         o.get("4:3", 0), o.get("5:4", 0),
                                         o.get("other", 0))))
    return 0


if __name__ == "__main__":
    sys.exit(main())

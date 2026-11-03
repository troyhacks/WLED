#!/usr/bin/env python3
"""THE OUT-OF-BAND PROBE, SPLIT: the slow side and the fast side.

The first attempt (_tempo_oob.py) compared the best candidate inside the
band with the best candidate outside it, over the WHOLE outside.  It had a
12% false-positive rate and a 0% detection rate.

This one separates the two sides, because the promise "the beat is between
118 and 176" is broken in two different ways and they may look different:

  bestBelow   the best RAW score among 60-118   (the music is slower)
  bestAbove   the best RAW score among 176-180  (the music is faster)

and reports, for the corpus windows whose truth is below the band, whether
bestBelow finds the truth.

    python tools/ar_sim/_tempo_oob2.py --limit 120
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

LO, HI = 60.0, 180.0
BAND_LO, BAND_HI = 118.0, 176.0
STEP = 0.25
BPMS = np.arange(LO, HI + 1e-6, STEP)
M_IN = (BPMS >= BAND_LO - 1e-6) & (BPMS <= BAND_HI + 1e-6)
M_BELOW = BPMS < BAND_LO - 1e-6
M_ABOVE = BPMS > BAND_HI + 1e-6


def one(key):
    o = oracle(key)
    if not o:
        return None
    tb = o[0]
    x = _cache.load(key)
    if x is None:
        return None
    odf = B.firmware_odf(x, 80.0, 320.0)
    configure(lo=BAND_LO, hi=BAND_HI, prior_weight=0.0)
    t = P.Tracker()
    t.run(odf)
    if t.sweep_frames is None or len(t.sweep_frames) < 8:
        return None
    sc, _ = t.comb_scores_all(t.sweep_frames, t.sweep_str, BPMS)
    sc = np.asarray(sc, dtype=float)
    if sc.size != BPMS.size:
        return None

    def best(mask):
        i = np.where(mask)[0]
        if i.size == 0:
            return None
        k = i[np.argmax(sc[i])]
        return float(BPMS[k]), float(sc[k])

    return dict(truth=float(tb), inside=bool(M_IN[np.argmin(np.abs(BPMS - tb))]),
                b_in=best(M_IN), b_below=best(M_BELOW), b_above=best(M_ABOVE))


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

    ins = [r for r in rows if r["inside"]]
    outs = [r for r in rows if not r["inside"]]
    print("=== the truth is INSIDE the band: the cry-wolf test, per side ===")
    for side in ("b_below", "b_above"):
        d = np.array([r[side][1] - r["b_in"][1] for r in ins if r[side]])
        print("   %-8s  n=%3d  median %+7.4f  max %+7.4f   wins in %d/%d (%.0f%%)"
              % (side, d.size, np.median(d), d.max(), int((d > 0).sum()),
                 d.size, 100.0 * (d > 0).mean()))
        for m in (0.0, 0.02, 0.05):
            print("              margin %+.2f -> fires on %d/%d (%.0f%%)"
                  % (m, int((d > m).sum()), d.size, 100.0 * (d > m).mean()))

    print()
    print("=== the truth is OUTSIDE the band: does the slow side find it? ===")
    for r in sorted(outs, key=lambda r: r["truth"]):
        bb = r["b_below"]
        ba = r["b_above"]
        print("   truth %7.2f | bestIn %7.2f (%.3f) | bestBelow %7.2f (%.3f) "
              "d=%+0.4f | bestAbove %7.2f (%.3f)"
              % (r["truth"], r["b_in"][0], r["b_in"][1], bb[0], bb[1],
                 bb[1] - r["b_in"][1], ba[0], ba[1]))

    print()
    print("=== for the INSIDE windows: how far below the band does the best")
    print("    slow candidate sit, and how much worse is it? ===")
    rel = np.array([r["b_below"][0] for r in ins])
    gap = np.array([r["b_below"][1] - r["b_in"][1] for r in ins])
    print("   bestBelow's rate: median %.1f  min %.1f  max %.1f"
          % (np.median(rel), rel.min(), rel.max()))
    print("   its score gap:    median %+0.4f  min %+0.4f  max %+0.4f"
          % (np.median(gap), gap.min(), gap.max()))
    # where does the bestBelow sit RELATIVE to the truth, when the truth is
    # inside the band?  if it is a clean fraction of the truth, it is an alias.
    rat = np.array([r["b_below"][0] / r["truth"] for r in ins])
    hist = {}
    for v in rat:
        k = round(v, 1)
        hist[k] = hist.get(k, 0) + 1
    print("   bestBelow / truth:",
          " ".join("%s:%d" % (k, hist[k]) for k in sorted(hist)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

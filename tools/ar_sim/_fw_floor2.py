#!/usr/bin/env python3
"""HOW LOW DOES THE COMB SCORE EVER GO?

AR_TEMPO_MIN_SCORE is the line the whole design stands on:

  * below it, the narrow search is said to have COLLAPSED - the beat it
    was tracking is not there any more - and a whole-band search is asked
    for;
  * below it, the tempo is not published as valid.

If the score never gets that low, then the collapse never happens, the
valid flag never goes false, and the whole-band search is only ever run by
the safety net.  That is worth knowing before the constant is set.

This measures, over the corpus:

  * the best candidate's score in each window - the ceiling
  * the narrow search's score through the window - the floor, which is what
    the collapse test actually sees
  * the lowest score the narrow search reaches, and how often it is below
    each candidate threshold

    python tools/ar_sim/_fw_floor2.py --windows 40
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402

THRESHOLDS = [0.05, 0.10, 0.15, 0.20, 0.25, 0.30]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=40)
    ap.add_argument("--band-lo", type=float, default=80.0)
    ap.add_argument("--band-hi", type=float, default=320.0)
    args = ap.parse_args()

    bests = []
    mins = []
    below = {t: 0 for t in THRESHOLDS}
    total_searches = 0
    n = 0
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, args.band_lo, args.band_hi)
        if len(odf) < 1200:
            continue
        t = P.Tracker()
        t.run(odf)
        if not t.locked:
            continue
        # the narrow search's score through the window, AFTER the first
        # lock - before that the score is still zero, because nothing has
        # been measured yet
        log = np.array(t.bpm_log)
        sc = np.array(t.score_log)
        if len(sc) < 100:
            continue
        locked_at = np.argmax(log > 0.0)
        if locked_at <= 0:
            continue
        sc = sc[locked_at:]
        bests.append(float(t.score))
        mins.append(float(sc.min()))
        for th in THRESHOLDS:
            below[th] += int(np.sum(sc < th))
        total_searches += len(sc)
        n += 1

    if not n:
        print("no usable windows")
        return 1
    b = np.array(bests)
    m = np.array(mins)
    print("%d windows, %d scored frames" % (n, total_searches))
    print()
    print("the best candidate's score in each window")
    print("   min %.3f   median %.3f   max %.3f" % (b.min(), np.median(b), b.max()))
    print()
    print("the LOWEST the narrow search's score gets in a window")
    print("   min %.3f   median %.3f   max %.3f" % (m.min(), np.median(m), m.max()))
    print()
    print("how often the score is BELOW a threshold, and so the collapse")
    print("   fires (the frames below the line / all the frames)")
    for th in THRESHOLDS:
        f = below[th]
        print("   %.2f   %8d frames   %6.3f %%   %d windows touched"
              % (th, f, 100.0 * f / total_searches,
                 sum(1 for v in m if v < th)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

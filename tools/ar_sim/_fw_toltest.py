#!/usr/bin/env python3
"""CAN THE COLLAPSE BRANCH BE REACHED AT A TIGHT TOLERANCE?

The claim is that AR_TEMPO_MIN_SCORE (0.10) sits below the comb score's
floor, so the collapse branch never fires.  The measurement behind that
claim used the DEFAULT tolerance (70 ms).

The tolerance is not a constant: AR_TEMPO_TOL_MIN_MS is 30, the serial 'e'
key moves it, and the settings API posts tempo.tol_ms.  A tighter tolerance
means a NARROWER box kernel, so it captures a smaller fraction of the onset
strength, so the score falls.

So: at every tolerance the board allows, how low does the narrow search's
score go?  The collapse branch needs it below 0.10.

    python tools/ar_sim/_fw_toltest.py --windows 40
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=40)
    ap.add_argument("--band-lo", type=float, default=80.0)
    ap.add_argument("--band-hi", type=float, default=320.0)
    ap.add_argument("--tols", default="30,40,50,70,100,150")
    args = ap.parse_args()

    tols = [float(t) for t in args.tols.split(",")]
    out = {}
    for tol in tols:
        P.TOL_MS = tol
        mins = []
        bests = []
        below10 = 0
        total = 0
        nw = 0
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
            log = np.array(t.bpm_log)
            sc = np.array(t.score_log)
            if len(sc) < 100:
                continue
            locked_at = np.argmax(log > 0.0)
            if locked_at <= 0:
                continue
            sc = sc[locked_at:]
            mins.append(float(sc.min()))
            bests.append(float(t.score))
            below10 += int(np.sum(sc < 0.10))
            total += len(sc)
            nw += 1
        if not nw:
            print("tol %.0f: no usable windows" % tol)
            continue
        m = np.array(mins)
        b = np.array(bests)
        out[tol] = (float(m.min()), float(np.median(m)), int(below10), int(total),
                    int(sum(1 for v in m if v < 0.10)))
        print("tol %5.0f ms   narrow floor: min %.3f  median %.3f   "
              "frames<0.10: %6d / %7d   windows touched: %d"
              % (tol, m.min(), np.median(m), below10, total,
                 sum(1 for v in m if v < 0.10)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

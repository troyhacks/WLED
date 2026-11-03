#!/usr/bin/env python3
"""THE SWEEPS' DECISIONS, ONE PER LINE.

Every whole-band sweep records the local rate and its score, the far rate
and its score, their ratio, and whether the far was taken.  This prints
them, so the veto's behaviour can be read directly instead of inferred
from a summary number.

    python tools/ar_sim/_fw_vetotrace2.py --windows 1 --rows 60
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
    ap.add_argument("--windows", type=int, default=1)
    ap.add_argument("--rows", type=int, default=60)
    ap.add_argument("--band-lo", type=float, default=80.0)
    ap.add_argument("--band-hi", type=float, default=320.0)
    ap.add_argument("--margin", type=float, default=0.20)
    ap.add_argument("--gain", type=float, default=0.10)
    args = ap.parse_args()

    n = 0
    seen = set()
    for k in _cache.keys():
        if k in seen:
            continue
        seen.add(k)
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, args.band_lo, args.band_hi)
        if len(odf) < 1200:
            continue
        t = P.Tracker()
        t.narrow_bpm = 2.0
        t.search_every = 4
        t.edge_rule = False
        t.collapse_trigger = True
        t.full_every = 32
        t.net_margin = args.margin
        t.pll_gain = args.gain
        t.run(odf)

        print("%s   (margin %.2f, gain %.2f)" % (k[:40], args.margin, args.gain))
        print("  frame  reason    bpm    local(bpm/score)     "
              "far(bpm/score)   ratio  far?")
        for r in t.trace[:args.rows]:
            print("  %6d  %-7s %7.2f  %7.2f/%7.4f  %7.2f/%7.4f  %6.3f  %s"
                  % (r["frame"], str(r["reason"]), r["bpm"],
                     r["local_bpm"], r["local_score"],
                     r["far_bpm"], r["far_score"], r["ratio"],
                     "FAR" if r["took_far"] else "-"))
        if len(t.trace) > args.rows:
            print("  ... %d more" % (len(t.trace) - args.rows))
        # the summary of the ratios
        if t.trace:
            ratios = np.array([r["ratio"] for r in t.trace])
            took = np.array([r["took_far"] for r in t.trace])
            print("  %d sweeps   ratio: median %.3f  p90 %.3f  max %.3f   "
                  "far taken %d"
                  % (len(t.trace), float(np.median(ratios)),
                     float(np.percentile(ratios, 90)), float(ratios.max()),
                     int(took.sum())))
            lo = ratios[~took]
            if len(lo):
                print("  the REFUSED ones: ratio median %.3f  max %.3f"
                      % (float(np.median(lo)), float(lo.max())))
        print()
        n += 1
        if n >= args.windows:
            break


if __name__ == "__main__":
    sys.exit(main())

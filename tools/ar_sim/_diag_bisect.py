#!/usr/bin/env python3
"""Bisect the firmware shape's loss.

The firmware's grid hits 0.741 over the last 12 s; the offline shape's grid
hits 0.870 over the same span.  Three things could be responsible and they
can be separated by mixing the pieces:

  A  the firmware's PEAKS, the offline's COMB, the offline's GRID
  B  the offline's PEAKS, the firmware's COMB, the firmware's GRID
  C  the firmware's PEAKS, the firmware's COMB, the firmware's GRID

If A is high and C is low, the loss is in the GRID (the PLL).  If A is low,
the loss is in the PEAKS or the COMB.

    python tools/ar_sim/_diag_bisect.py --limit 8
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _beat as B                                  # noqa: E402
import _comb as C                                  # noqa: E402
from _device_odf import device_odf, DEVICE_RATE    # noqa: E402
import _fw_sim as F                                # noqa: E402
from _diag_fw import firmware_peaks                # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402
from _eval_tempo import hit_with_offset, grid_at   # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=8)
    args = ap.parse_args()

    tot = {"A": [], "B": [], "C": [], "D": [], "E": [], "F": []}
    for k in _cache.keys()[:args.limit]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = device_odf(x)
        if len(odf) < 1200:
            continue
        win = int(12.0 * DEVICE_RATE)
        first = max(0, len(odf) - win)

        # the raw onsets, over the last 12 s
        t_hi = len(odf) / DEVICE_RATE
        t_lo = first / DEVICE_RATE
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        wo = ons[(ons >= t_lo - 0.02) & (ons <= t_hi)]
        if len(wo) < 4:
            continue

        # the offline peaks, restricted to the last 12 s
        mx = float(np.max(odf))
        odfn = odf / mx if mx > 0 else odf
        op, os_ = B.peak_pick(odfn)
        keep = op >= first
        op, os_ = op[keep], os_[keep]
        if len(op) < 8:
            continue

        # the firmware peaks, restricted to the last 12 s
        fp = firmware_peaks(odf)
        fp = [(a, b) for a, b in fp if a >= first]
        if len(fp) < 8:
            continue
        ffr = np.array([p[0] for p in fp], dtype=float)
        fsg = np.array([p[1] for p in fp], dtype=float)

        # the two combs
        obpm, oscore, ophase, _d = C.comb_rate(op, os_, step=0.25,
                                                prior_bpm=F.PRIOR_BPM,
                                                prior_weight=F.PRIOR_WEIGHT)
        fbpm, fscore, fphase = F.comb_search(ffr, fsg)

        # E: the firmware's comb, but a FULL 0.25 BPM sweep at 64 bins
        ebpm, escore, ephase = F.comb_full(ffr, fsg)

        # F: the firmware's comb, but 256 bins (coarse-to-fine kept)
        F.NBINS = 256
        gbpmb, gscoreb, gphaseb = F.comb_search(ffr, fsg)
        F.NBINS = 64

        # A: the firmware's peaks, the offline's comb, the offline's grid
        ga = grid_at(obpm, ophase, first, len(odf))
        fa, _o = hit_with_offset(obpm, ga / DEVICE_RATE, wo)

        # B: the offline's peaks, the firmware's comb, the offline's grid
        gb = grid_at(fbpm, fphase, first, len(odf))
        fb, _o = hit_with_offset(fbpm, gb / DEVICE_RATE, wo)

        # C: the firmware's peaks, the firmware's comb, the offline's grid
        gc = grid_at(fbpm, fphase, first, len(odf))
        fc, _o = hit_with_offset(fbpm, gc / DEVICE_RATE, wo)

        # D: the offline's peaks, the offline's comb, the offline's grid
        gd = grid_at(obpm, ophase, first, len(odf))
        fd, _o = hit_with_offset(obpm, gd / DEVICE_RATE, wo)

        # E: 64 bins, full sweep
        ge = grid_at(ebpm, ephase, first, len(odf))
        fe, _o = hit_with_offset(ebpm, ge / DEVICE_RATE, wo)

        # F: 256 bins, coarse-to-fine
        gf = grid_at(gbpmb, gphaseb, first, len(odf))
        ff, _o = hit_with_offset(gbpmb, gf / DEVICE_RATE, wo)

        tot["A"].append(fa)
        tot["B"].append(fb)
        tot["C"].append(fc)
        tot["D"].append(fd)
        tot["E"].append(fe)
        tot["F"].append(ff)
        print("  %-38s off %6.2f  fw %6.2f  64full %6.2f  256c2f %6.2f   "
              "A %4.2f B %4.2f C %4.2f D %4.2f E %4.2f F %4.2f"
              % (k[:38], obpm, fbpm, ebpm, gbpmb, fa, fb, fc, fd, fe, ff))

    print()
    names = {"A": "fw peaks + off comb (256, full)",
             "B": "off peaks + fw comb (64, c2f)",
             "C": "fw peaks + fw comb (64, c2f)",
             "D": "off peaks + off comb (256, full)",
             "E": "fw peaks + fw comb (64, FULL sweep)",
             "F": "fw peaks + fw comb (256, c2f)"}
    for key in ("D", "A", "B", "C", "E", "F"):
        a = np.array(tot[key])
        print("  %-36s median %5.3f   >=0.5: %5.1f%%"
              % (names[key], float(np.median(a)), 100.0 * float((a >= 0.5).mean())))


if __name__ == "__main__":
    sys.exit(main())

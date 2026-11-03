#!/usr/bin/env python3
"""The band, the peak set, the bin count - and the missing cell.

Four questions, all on the SAME tracks:

  1. THE BAND.  The emulation measured an 80-320 Hz band and the firmware
     will measure 40-160 Hz.  Which is better on the corpus?

  2. THE PEAK SET.  The firmware's picker finds ~20 % more peaks than the
     offline's.  Is the loss in the peak set, or in the comb?

  3. THE MISSING CELL.  The bisect measured "offline peaks + offline comb"
     and "firmware peaks + firmware comb", but never "offline peaks +
     firmware's comb".  If that cell is HIGH, the peak set is innocent and
     the COMB is the difference.  If it is LOW, the peak set is the
     difference.

  4. THE STRENGTH SCALE.  The offline normalises the onset strengths by
     their MEDIAN; the firmware normalises by the ODF's running maximum.
     The comb score is a fraction of the TOTAL strength, so the weighting
     decides which phase wins: by the median every onset counts about the
     same, by the running max the loudest onset dominates.

    python tools/ar_sim/_diag_band.py --limit 25
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
from _device_odf2 import device_odf_fw              # noqa: E402
import _fw_sim as F                                # noqa: E402
from _diag_fw import firmware_peaks                # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402
from _eval_tempo import hit_with_offset, grid_at   # noqa: E402

WIN = int(12.0 * DEVICE_RATE)


def offline_shape(odf, first):
    """The offline picker + the offline comb + a static grid."""
    mx = float(np.max(odf))
    odfn = odf / mx if mx > 0 else odf
    op, os_ = B.peak_pick(odfn)
    keep = op >= first
    op, os_ = op[keep], os_[keep]
    if len(op) < 8:
        return None
    bpm, score, phase, _d = C.comb_rate(op, os_, step=0.25,
                                        prior_bpm=F.PRIOR_BPM,
                                        prior_weight=F.PRIOR_WEIGHT)
    g = grid_at(bpm, phase, first, len(odf))
    return bpm, g


def firmware_shape(odf, first):
    """The firmware's frame loop: its picker, its comb, its PLL."""
    state = F.TempoState()
    beats, peaks = F.firmware_frame_loop(odf, state)
    if not len(beats):
        return None
    b12 = np.array([b for b in beats if b >= first], dtype=float)
    return state.bpm, b12


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=25)
    args = ap.parse_args()

    names = ["off/old", "off/new", "fw/old", "fw/new",
             "offPEAKS+fwCOMB/new", "fwPEAKS+medSTR/new"]
    tot = {n: [] for n in names}

    for k in _cache.keys()[:args.limit]:
        x = _cache.load(k)
        if x is None:
            continue
        odf_old = device_odf(x)
        odf_new = device_odf_fw(x)
        if len(odf_new) < 1200:
            continue

        first = max(0, len(odf_new) - WIN)
        t_hi = len(odf_new) / DEVICE_RATE
        t_lo = first / DEVICE_RATE
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        wo = ons[(ons >= t_lo - 0.02) & (ons <= t_hi)]
        if len(wo) < 4:
            continue

        line = "  %-40s" % k[:40]

        # --- 1 and 2: the offline shape on each band ----------------------
        for tag, odf in (("old", odf_old), ("new", odf_new)):
            r = offline_shape(odf, first)
            if r is None:
                line += "  %s  --   " % tag
                continue
            bpm, g = r
            f, _o = hit_with_offset(bpm, g / DEVICE_RATE, wo)
            tot["off/" + tag].append(f)
            line += "  %s %6.2f %4.2f" % (tag, bpm, f)

        # --- 3 and 4: the firmware shape on each band ---------------------
        for tag, odf in (("old", odf_old), ("new", odf_new)):
            r = firmware_shape(odf, first)
            if r is None:
                line += "  %s  --   " % tag
                continue
            bpm, b12 = r
            if not len(b12):
                line += "  %s %6.2f  0.00" % (tag, bpm)
                tot["fw/" + tag].append(0.0)
                continue
            f, _o = hit_with_offset(bpm, b12 / DEVICE_RATE, wo)
            tot["fw/" + tag].append(f)
            line += "  %s %6.2f %4.2f" % (tag, bpm, f)

        # --- 5: the MISSING CELL - the offline's peaks, the firmware's comb
        mx = float(np.max(odf_new))
        odfn = odf_new / mx if mx > 0 else odf_new
        op, os_ = B.peak_pick(odfn)
        keep = op >= first
        op, os_ = op[keep], os_[keep]
        if len(op) >= 8:
            fbpm, fscore, fphase = F.comb_full(op, os_)
            g = grid_at(fbpm, fphase, first, len(odf_new))
            f, _o = hit_with_offset(fbpm, g / DEVICE_RATE, wo)
            tot["offPEAKS+fwCOMB/new"].append(f)
            line += "  O+F %6.2f %4.2f" % (fbpm, f)

        # --- 6: the firmware's peaks, but the strengths by their MEDIAN ----
        fp = firmware_peaks(odf_new)
        fp = [(a, b) for a, b in fp if a >= first]
        if len(fp) >= 8:
            fr = np.array([p[0] for p in fp], dtype=float)
            sg = np.array([p[1] for p in fp], dtype=float)
            med = float(np.median(sg))
            if med > 0:
                sg = sg / med
            mbpm, mscore, mphase = F.comb_full(fr, sg)
            g = grid_at(mbpm, mphase, first, len(odf_new))
            f, _o = hit_with_offset(mbpm, g / DEVICE_RATE, wo)
            tot["fwPEAKS+medSTR/new"].append(f)
            line += "  M %6.2f %4.2f" % (mbpm, f)

        print(line)

    print()
    for n in names:
        a = np.array(tot[n])
        if not len(a):
            continue
        print("  %-24s median %5.3f   >=0.5: %5.1f%%   >=0.8: %5.1f%%"
              % (n, float(np.median(a)), 100.0 * float((a >= 0.5).mean()),
                 100.0 * float((a >= 0.8).mean())))


if __name__ == "__main__":
    sys.exit(main())

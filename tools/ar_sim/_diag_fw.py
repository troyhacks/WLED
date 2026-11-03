#!/usr/bin/env python3
"""Where does the firmware shape lose the phase?

The firmware shape hit 0.710 where the offline shape hit 0.870, and every
BPM it reported was RIGHT.  So the rate is not the problem - the phase is.
Three things could move it, and this separates them:

  picker   the firmware's peak picker finds a different set of peaks
           (count, positions) than the offline one
  comb     given the SAME peaks, the firmware's 64-bin coarse-to-fine
           search finds a different phase than the offline's 256-bin
           full sweep
  pll      given the SAME comb result, the PLL's grid drifts away from
           the comb's own grid

Each is measured on the same tracks, one at a time.

    python tools/ar_sim/_diag_fw.py --limit 12
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


def firmware_peaks(odf):
    """The firmware's peak picker, without the PLL or the search."""
    hist = []
    peaks = []
    prev_peak_frame = -1000
    peak_strength = 0.0
    odf_max = 0.0
    for i in range(len(odf)):
        v = float(odf[i])
        if v > odf_max:
            odf_max = v
        else:
            odf_max *= F.MAX_DECAY
        hist.append(v)
        if len(hist) > F.HIST_LEN:
            hist.pop(0)
        j = i - F.PEAK_POST
        if j >= F.PEAK_PRE and len(hist) >= F.HIST_LEN:
            base = len(hist) - F.PEAK_POST - 1
            oj = hist[base]
            seg = hist[base - F.PEAK_PRE: base + F.PEAK_POST + 1]
            a = max(0, base - F.PEAK_MEAN_HALF)
            mj = float(np.mean(hist[a:base + 1]))
            on = oj / odf_max if odf_max > 0 else 0.0
            if on >= F.PEAK_FLOOR and oj >= max(seg) \
                    and oj >= (1.0 + F.PEAK_DELTA) * mj:
                if j - prev_peak_frame < F.PEAK_MIN_GAP:
                    if on > peak_strength:
                        prev_peak_frame = j
                        peak_strength = on
                        peaks[-1] = (j, on)
                else:
                    prev_peak_frame = j
                    peak_strength = on
                    peaks.append((j, on))
    return peaks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=12)
    args = ap.parse_args()

    for k in _cache.keys()[:args.limit]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = device_odf(x)
        if len(odf) < 1200:
            continue

        # 1. the offline picker
        mx = float(np.max(odf))
        odfn = odf / mx if mx > 0 else odf
        op, os_ = B.peak_pick(odfn)
        # the offline comb: 256 bins, the full 0.25 BPM sweep
        obpm, oscore, ophase, _d = C.comb_rate(op, os_, step=0.25,
                                                prior_bpm=F.PRIOR_BPM,
                                                prior_weight=F.PRIOR_WEIGHT)

        # 2. the firmware picker
        fp = firmware_peaks(odf)
        ffr = np.array([p[0] for p in fp], dtype=float)
        fsg = np.array([p[1] for p in fp], dtype=float)

        # 3. the firmware comb, on the FIRMWARE's peaks
        fbpm, fscore, fphase = F.comb_search(ffr, fsg)

        # 4. the offline comb, on the FIRMWARE's peaks
        nbpm, nscore, nphase, _d = C.comb_rate(ffr, fsg, step=0.25,
                                               prior_bpm=F.PRIOR_BPM,
                                               prior_weight=F.PRIOR_WEIGHT)

        print(k[:44])
        print("   offline picker  %4d peaks    offline comb %7.2f score %4.2f "
              "phase %7.2f" % (len(op), obpm, oscore, ophase))
        print("   firmware picker %4d peaks    firmware comb %7.2f score %4.2f "
              "phase %7.2f" % (len(fp), fbpm, fscore, fphase))
        print("                      on the FIRMWARE peaks: %7.2f score %4.2f "
              "phase %7.2f" % (nbpm, nscore, nphase))
        if len(op) and len(fp):
            d = np.abs(op[:, None] - np.array([p[0] for p in fp])[None, :]).min(axis=1)
            print("   of the offline peaks, %4d have a firmware peak within "
                  "3 frames (%.0f%%)" % (int((d <= 3).sum()), 100.0 * (d <= 3).mean()))
        print()


if __name__ == "__main__":
    sys.exit(main())

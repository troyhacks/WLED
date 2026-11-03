#!/usr/bin/env python3
"""WHY DOES THE FAR CANDIDATE BEAT THE LOCAL ONE?

The veto compares two maxima: the best evidence inside a 17-candidate narrow
window, and the best evidence outside it - over 324 candidates.  A maximum
over 324 draws is biased UPWARD against a maximum over 17, and the bias may
well be bigger than the margin.

This takes ONE locked snapshot of the tracker and prints:

  * the score at the rate the tracker is holding
  * the best score anywhere in the band, and the rate that holds it
  * the score at every local maximum of the score curve, so the SHAPE of the
    curve is visible: how many rates compete, and by how much

The shape is the thing.  If the curve has a broad, flat top - a dozen rates
within a few per cent of each other - then no margin on a single maximum
can ever separate them, and the fix has to be something other than a margin.

    python tools/ar_sim/_fw_vetoscope2.py --windows 2
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402


def snapshot(t, odf):
    """Run the tracker with the veto disabled and take its onset set."""
    t.narrow_bpm = 2.0
    t.search_every = 4
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = 32
    t.net_margin = 100.0
    t.pll_gain = 0.10
    t.run(odf)
    # the onsets the picker found
    frames = np.array([f for f, _ in t.peak_frame], dtype=float)
    strs = np.array([s for _, s in t.peak_frame], dtype=float)
    return frames, strs, t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=2)
    ap.add_argument("--band-lo", type=float, default=80.0)
    ap.add_argument("--band-hi", type=float, default=320.0)
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
        frames, strs, t = snapshot(t, odf)
        if len(frames) < 8:
            continue
        bpms = P.BPM_LO + np.arange(P.CAND_COUNT) * P.CAND_STEP
        scores, _ph = t.comb_scores_all(frames, strs, bpms)

        held = t.bpm
        jh = int(np.argmin(np.abs(bpms - held)))
        jf = int(np.argmax(scores))
        print("%s" % k[:40])
        print("   held %.2f BPM  score %.4f   (rank %d of %d)"
              % (bpms[jh], scores[jh], int(np.sum(scores > scores[jh])) + 1,
                 len(scores)))
        print("   best  %.2f BPM  score %.4f" % (bpms[jf], scores[jf]))
        print("   far/local score ratio  %.4f   (the margin needs > %.4f)"
              % (scores[jf] / max(scores[jh], 1e-9), 1.0 + 0.20))
        # the shape: the local maxima of the score curve
        loc = []
        for i in range(1, len(scores) - 1):
            if scores[i] >= scores[i - 1] and scores[i] > scores[i + 1]:
                loc.append((scores[i], bpms[i]))
        loc.sort(reverse=True)
        print("   %d local maxima in the band; the best 12:" % len(loc))
        for s, b in loc[:12]:
            print("      %7.2f BPM  %.4f   %+.4f vs held"
                  % (b, s, s - scores[jh]))
        # how many of them are within the margin of the best?
        within = sum(1 for s, b in loc if s >= loc[0][0] * 0.80)
        print("   %d local maxima within 20 %% of the best" % within)
        print()
        n += 1
        if n >= args.windows:
            break


if __name__ == "__main__":
    sys.exit(main())

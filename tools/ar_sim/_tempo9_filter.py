#!/usr/bin/env python3
"""Experiment 9 (fast): the front end, with the final selection rule.

The front end is the only expensive stage of the design (biquads over 512
samples a frame), so the choice of filter has to be measured - and it has
to be measured with the selection rule the firmware will use.

The ODF and the magnitudes are computed once per (track, design).

    python tools/ar_sim/_tempo9_filter.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _tempo5_sim as t5                                            # noqa: E402
from _tempo2_sim import STYLES                                      # noqa: E402
from _tempo3_sim import filter_designs, rbj_bpf, rbj_hpf, rbj_lpf, SR, BUTTER_Q

BPMS = (90, 100, 120, 128, 140, 150, 160, 174)

EXTRA = {
    "bpf85_1sec": [rbj_bpf(85.0, SR, 1.5)],
    "bpf70_1sec": [rbj_bpf(70.0, SR, 1.5)],
    "hp40_lp200": [rbj_hpf(40.0, SR, BUTTER_Q), rbj_lpf(200.0, SR, BUTTER_Q)],
}

CACHE = {}


def mags_for(style, bpm, name, sections):
    key = (style, bpm, name)
    if key not in CACHE:
        x = t5.get_track(style, bpm, 0.0, 0.0)
        odf = t5.odf_fast(x, sections)
        CACHE[key] = (odf, t5.running_dft_grid(odf))
    return CACHE[key]


def main():
    ds = dict(filter_designs())
    ds.update(EXTRA)
    for style in STYLES:
        for bpm in BPMS:
            for name in ds:
                if name != "ideal":
                    mags_for(style, bpm, name, ds[name])
    print("cache warm: %d" % len(CACHE))
    print("%-14s %6s %6s  %-30s %s" %
          ("design", "tie3", "tie0", "per-style (tie0)", "worst(tie0)"))
    for name in ds:
        if name == "ideal":
            continue
        res = {}
        for tie in (3.0, 0.0):
            wrong = 0
            per = {}
            worst = 0.0
            for style in STYLES:
                w = 0
                for bpm in BPMS:
                    odf, mags = mags_for(style, bpm, name, ds[name])
                    klo, khi = t5.band_bins(90.0, 175.0)
                    bpm_est, mag = t5.peak_strict(mags, klo, khi)
                    if tie > 0.0:
                        bpm_est = t5.rational_slower(bpm_est, mag, mags, klo, khi, tie)
                    if bpm_est is None or abs(bpm_est - bpm) >= 2.0:
                        w += 1
                        wrong += 1
                        worst = max(worst, abs((bpm_est or 0) - bpm))
                per[style] = w
            res[tie] = (wrong, per, worst)
        w3, p3, x3 = res[3.0]
        w0, p0, x0 = res[0.0]
        print("%-14s %6d %6d  %-30s %.2f" %
              (name, w3, w0,
               " ".join("%s:%d" % (k[:5], v) for k, v in p0.items()), x0))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""THE COVERAGE: which corpus windows each band covers, at the catalogue-fast level.

THE QUESTION
------------
A band [Bmin, Bmax] covers a track if the track's CATALOGUE tempo - the
level the DJ counts - is inside it.  The catalogue level is the FAST one:
a house track whose kick lands every 0.8 s is catalogued at 128 BPM, not
at 64.

So a window is covered by a band if the oracle's BPM is inside the band, OR
the oracle's DOUBLE is inside it (the oracle reported the half-time).

    covered   the catalogue-fast level is inside the band
    clipped   the fast level is BELOW the band  (the band reports a double)
    over      the fast level is ABOVE the band  (the band reports a half)

    python tools/ar_sim/_tempo_cover.py
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _tempo_range import oracle                   # noqa: E402

ORACLE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_oracle")


def all_oracles():
    out = []
    for fn in sorted(os.listdir(ORACLE_DIR)):
        if not fn.endswith(".json"):
            continue
        key = fn[:-5]
        o = oracle(key)
        if not o:
            continue
        out.append((key, o[0], o[1]))
    return out


BANDS = [
    ("60-180  full",       60.0, 180.0),
    ("64-94   slow",       64.0,  94.0),
    ("70-140  slow",      70.0, 140.0),
    ("88-176  wide",      88.0, 176.0),
    ("90-175  shipped",   90.0, 175.0),
    ("100-176",          100.0, 176.0),
    ("118-176  club",    118.0, 176.0),
    ("120-176  fast",    120.0, 176.0),
    ("132-176  tight",   132.0, 176.0),
]


def main():
    data = all_oracles()
    print("%d windows" % len(data))
    bpms = sorted(x[1] for x in data)
    print("oracle: min %.2f  max %.2f  median %.2f  mean %.2f"
          % (bpms[0], bpms[-1], bpms[len(bpms) // 2], sum(bpms) / len(bpms)))

    print("\n%-18s %6s  %8s %8s %8s   %s" % ("band", "g", "covered",
                                             "clipped", "over", "alias ladder"))
    for (label, lo, hi) in BANDS:
        cov = clip = over = 0
        for (k, bpm, beats) in data:
            fast = bpm
            if fast < lo / 2.0:
                fast = 2.0 * bpm
            if fast < lo:
                clip += 1
            elif fast > hi:
                over += 1
            else:
                cov += 1
        g = lo - hi / 2.0
        ladder = []
        for r, name in ((2.0, "2:1"), (1.5, "3:2"), (4.0 / 3.0, "4:3"),
                        (1.25, "5:4")):
            if hi >= r * lo:
                ladder.append(name)
        print("%-18s %+6.1f  %8d %8d %8d   %s"
              % (label, g, cov, clip, over,
                 "free of " + ", ".join(ladder) if ladder else "-"))

    print("\n=== the windows 118-176 does NOT cover, and why ===")
    miss = []
    for (k, bpm, beats) in data:
        fast = bpm if bpm >= 59.0 else 2.0 * bpm
        if not (118.0 <= fast <= 176.0):
            miss.append((fast, bpm, k))
    miss.sort()
    for (fast, bpm, k) in miss:
        print("  %-34s oracle %7.2f  fast %7.2f" % (k, bpm, fast))
    print("  %d windows missed of %d" % (len(miss), len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""THE MARGIN AFTER A CUT, WITH THE GRID PINNED.

The comb looks at TWELVE SECONDS of onsets.  Right after the music
changes, that window is still full of the OLD music, so the old rate
still scores well and the new one scores badly - the margin is tiny and
the change is invisible.

As the window turns over the picture has to change: the old rate's score
falls and the new one's rises.  This traces that, with the grid PINNED
to the old rate (the veto always on) so the trace shows the landscape
and not the tracker's reaction to it.

If the margin does clear a threshold as the window turns over, the veto
is usable: it rejects the near-ties on unchanged music and accepts the
real change once the window has turned over.

    python tools/ar_sim/_fw_evolve.py --windows 30 --pairs 10
"""

import argparse
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402

ALL_BPMS = P.BPM_LO + np.arange(P.CAND_COUNT) * P.CAND_STEP
NARROW = 2.0


def landscape(t, frame, rate):
    """(far bpm, far score, local bpm, local score, margin) on one snapshot."""
    cutoff = frame - P.COMB_FRAMES
    keep = [(f, s) for (f, s) in t.peak_frame if f >= cutoff]
    if len(keep) < P.MIN_PEAKS:
        return None
    frames = np.array([f for f, _ in keep], dtype=float)
    strs = np.array([s for _, s in keep], dtype=float)
    scores, _ph = t.comb_scores_all(frames, strs, ALL_BPMS)
    values = scores - t.prior
    lo = max(P.BPM_LO, rate - NARROW)
    hi = min(P.BPM_HI, rate + NARROW)
    f0 = int(np.ceil((lo - P.BPM_LO) / P.CAND_STEP))
    c0 = int(np.floor((hi - P.BPM_LO) / P.CAND_STEP)) - f0 + 1
    idx = np.arange(f0, f0 + c0)
    jn = int(idx[np.argmax(values[idx])])
    jg = int(np.argmax(values))
    inside = (f0 <= jg <= f0 + c0 - 1)
    return (float(ALL_BPMS[jg]), float(scores[jg]),
            float(ALL_BPMS[jn]), float(scores[jn]),
            float(scores[jg] - scores[jn]), inside)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=30)
    ap.add_argument("--pairs", type=int, default=10)
    ap.add_argument("--lo", type=float, default=80.0)
    ap.add_argument("--hi", type=float, default=320.0)
    args = ap.parse_args()

    t0 = time.time()
    items = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, args.lo, args.hi)
        if len(odf) < 1200:
            continue
        items.append((k, odf))
    print("%d windows   %.0f s" % (len(items), time.time() - t0), flush=True)

    def ref(odf):
        t = P.Tracker()
        t.narrow_bpm = 0.0
        t.run(odf)
        return t.bpm if t.locked and t.bpm > 0 else None

    print()
    print("THE MARGIN AFTER A CUT, GRID PINNED AT THE OLD RATE")
    for i in range(len(items)):
        bpm_a = ref(items[i][1])
        if bpm_a is None:
            continue
        for j in range(i + 1, len(items)):
            bpm_b = ref(items[j][1])
            if bpm_b is None or abs(bpm_b - bpm_a) < 6.0:
                continue
            odf_a, odf_b = items[i][1], items[j][1]
            both = np.concatenate((odf_a, odf_b))
            cut = len(odf_a)
            t = P.Tracker()
            t.narrow_bpm = NARROW
            t.search_every = 4
            t.edge_rule = False
            t.collapse_trigger = True
            t.full_every = 32
            t.net_margin = 99.0        # the veto always rejects: the grid stays
            row = []
            for k2 in range(len(both)):
                t.frame(float(both[k2]))
                if k2 >= cut and (k2 - cut) % 16 == 0 and t.locked:
                    m = landscape(t, t.frame_index, bpm_a)
                    if m is None:
                        continue
                    row.append(((k2 - cut) / P.RATE, m[0], m[1], m[2], m[3],
                                m[4], m[5]))
            print()
            print("  %-28s (%6.2f) -> %-28s (%6.2f)"
                  % (items[i][0][:28], bpm_a, items[j][0][:28], bpm_b))
            for r in row:
                tag = "  (inside)" if r[6] else ""
                print("      +%5.1f s  far %6.2f %.3f   local %6.2f %.3f"
                      "   margin %+.3f%s" % (r[0], r[1], r[2], r[3], r[4], r[5],
                                            tag))
            break
        if i >= 4:
            break

    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

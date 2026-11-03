#!/usr/bin/env python3
"""THE OUT-OF-BAND PROBE: does it work, and does it cry wolf?

THE QUESTION
-------------
The tracker must be able to say "this music is not in the range you
promised".  That means comparing the best candidate INSIDE the band with
the best candidate OUTSIDE it, on raw evidence (the prior excluded).

Two failure modes, and they pull in opposite directions:

  MISSED   the music is at 95 BPM, the band is 118-176, and the band's
           best candidate is the 4:3 alias at 126.7.  If the alias beats
           the truth on raw evidence (measured: it does, by 0.016-0.027)
           then no margin ever fires and the report is blind.

  CRY WOLF the music is at 128 BPM, inside the band, and some candidate
           outside the band (the half-time at 64, or the 3:4 alias at 96)
           scores higher than the truth.  Then the report fires on a
           perfectly good track.

So this measures, for every corpus window, with the club band 118-176:

  bestIn    the best RAW score among the candidates inside the band
  bestOut   the best RAW score among the candidates outside it
  bestAll   the best RAW score over the whole 60-180 search range
  truth     the catalogue-fast oracle tempo

and reports the gap bestOut - bestIn for windows whose truth is INSIDE
the band (the false-positive margin) and the same for windows whose truth
is OUTSIDE (the sensitivity margin).

    python tools/ar_sim/_tempo_oob.py --limit 120
"""

import argparse
import multiprocessing as mp
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402
from _tempo_range import configure, oracle         # noqa: E402

LO, HI = 60.0, 180.0
BAND_LO, BAND_HI = 118.0, 176.0
STEP = 0.25
BPMS = np.arange(LO, HI + 1e-6, STEP)
IN = (BPMS >= BAND_LO - 1e-6) & (BPMS <= BAND_HI + 1e-6)


def one(key):
    o = oracle(key)
    if not o:
        return None
    tb = o[0]
    x = _cache.load(key)
    if x is None:
        return None
    odf = B.firmware_odf(x, 80.0, 320.0)
    configure(lo=BAND_LO, hi=BAND_HI, prior_weight=0.0)
    t = P.Tracker()
    t.run(odf)
    if t.sweep_frames is None or len(t.sweep_frames) < 8:
        return None
    sc, _ = t.comb_scores_all(t.sweep_frames, t.sweep_str, BPMS)
    sc = np.asarray(sc, dtype=float)
    if sc.size != BPMS.size:
        return None
    i_in = np.where(IN)[0]
    i_out = np.where(~IN)[0]
    if i_in.size == 0 or i_out.size == 0:
        return None
    k_in = i_in[np.argmax(sc[i_in])]
    k_out = i_out[np.argmax(sc[i_out])]
    k_all = int(np.argmax(sc))
    return dict(
        truth=float(tb),
        inside=bool(BAND_LO <= tb <= BAND_HI),
        s_in=float(sc[k_in]), b_in=float(BPMS[k_in]),
        s_out=float(sc[k_out]), b_out=float(BPMS[k_out]),
        s_all=float(sc[k_all]), b_all=float(BPMS[k_all]),
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=120)
    args = ap.parse_args()
    keys = _cache.keys()[:args.limit]

    rows = []
    with mp.Pool(8) as pool:
        for r in pool.imap(one, keys):
            if r:
                rows.append(r)
    print("%d windows\n" % len(rows))

    ins = [r for r in rows if r["inside"]]
    outs = [r for r in rows if not r["inside"]]

    print("=== the truth is INSIDE the band 118-176  (the cry-wolf test) ===")
    print("   the margin bestOut - bestIn; a NEGATIVE margin means the")
    print("   out-of-band candidate is worse and the report is quiet.")
    d = np.array([r["s_out"] - r["s_in"] for r in ins])
    if d.size:
        print("   n=%3d  median %+7.4f  mean %+7.4f  max %+7.4f"
              % (d.size, np.median(d), d.mean(), d.max()))
        print("   the out-of-band candidate WINS in %d/%d (%.0f%%)"
              % (int((d > 0).sum()), d.size, 100.0 * (d > 0).mean()))
        for m in (0.0, 0.02, 0.05, 0.10, 0.12):
            print("   margin %+.2f -> the report fires on %d/%d (%.0f%%)"
                  % (m, int((d > m).sum()), d.size, 100.0 * (d > m).mean()))

    print()
    print("=== the truth is OUTSIDE the band  (the sensitivity test) ===")
    print("   the margin bestOut - bestIn; a POSITIVE margin means the")
    print("   out-of-band candidate wins and the report fires.")
    d2 = np.array([r["s_out"] - r["s_in"] for r in outs])
    if d2.size:
        print("   n=%3d  median %+7.4f  mean %+7.4f  min %+7.4f"
              % (d2.size, np.median(d2), d2.mean(), d2.min()))
        for m in (0.0, 0.02, 0.05, 0.10, 0.12):
            print("   margin %+.2f -> the report fires on %d/%d (%.0f%%)"
                  % (m, int((d2 > m).sum()), d2.size, 100.0 * (d2 > m).mean()))

    print()
    print("=== the unrestricted winner: is it inside the band? ===")
    for label, group in (("truth inside", ins), ("truth outside", outs)):
        if not group:
            continue
        won = sum(1 for r in group if BAND_LO <= r["b_all"] <= BAND_HI)
        print("   %-14s the unrestricted winner is INSIDE the band in "
              "%d/%d (%.0f%%)"
              % (label, won, len(group), 100.0 * won / len(group)))
    print()
    print("=== the misses, in detail (the truth is outside the band) ===")
    for r in sorted(outs, key=lambda r: r["s_out"] - r["s_in"]):
        print("   truth %7.2f  bestIn %7.2f (%.3f)  bestOut %7.2f (%.3f)  "
              "bestAll %7.2f (%.3f)  margin %+0.4f"
              % (r["truth"], r["b_in"], r["s_in"], r["b_out"], r["s_out"],
                 r["b_all"], r["s_all"], r["s_out"] - r["s_in"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())

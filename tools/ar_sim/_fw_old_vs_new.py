#!/usr/bin/env python3
"""THE OLD TRACKER AND THE NEW ONE, SIDE BY SIDE.

The tracker was rebuilt - a comb whose kernel is fixed in TIME, a narrow
search with a whole-band safety net, a proportional PLL and a veto on the
safety net.  The rebuild was measured against librosa's beat tracker and it
agrees with it.  But it also scored LOWER against the raw onsets than the
tracker it replaced, and that needs an explanation rather than a shrug.

So this runs both, on the SAME windows, with the SAME instruments, and adds
the third case that separates the two changes that were made at once:

    OLD algorithm, 40-160 Hz   the tracker as it was
    NEW algorithm, 40-160 Hz   the new one, on the OLD band's audio
    NEW algorithm, 80-320 Hz   the new one, on the band it ships with

and the reference, librosa's beat tracker, scored the same way.

    python tools/ar_sim/_fw_old_vs_new.py --windows 120
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
import _fw_sim as S                                # noqa: E402
import _oracle_librosa as O                        # noqa: E402
from _diag_phase import kick_onsets                # noqa: E402
from _device_odf import device_odf                # noqa: E402


def new_tracker(margin=0.70, gain=0.10, prior=0.6, narrow_prior=False):
    t = P.Tracker()
    t.narrow_bpm = 2.0
    t.search_every = 4
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = 32
    t.net_margin = margin
    t.pll_gain = gain
    t.prior_weight = prior
    t.prior = t._prior_table()
    t.prior_in_narrow = narrow_prior
    return t


def rate_err(published, truth):
    """The distance to the truth, allowing the METRICAL relation."""
    errs, levels = [], []
    for t in published:
        d = [abs(t - truth), abs(t - 2.0 * truth), abs(t - truth * 0.5)]
        errs.append(min(d))
        levels.append(int(np.argmin(d)))
    return float(np.median(errs)), levels


def wander(beats_bpm):
    if len(beats_bpm) < 50:
        return 0.0, 0
    half = np.array(beats_bpm[len(beats_bpm) // 2:])
    half = half[half > 0]
    if len(half) < 50:
        return 0.0, 0
    return (float(np.std(half)),
            int(np.sum(np.abs(np.diff(half)) > 0.5)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=120)
    ap.add_argument("--margin", type=float, default=0.70)
    ap.add_argument("--gain", type=float, default=0.10)
    ap.add_argument("--prior", type=float, default=0.6)
    args = ap.parse_args()

    t0 = time.time()
    cases = ("old", "new40", "new80")
    data = {c: {"kick": [], "wide": [], "lib": [], "err": [], "wander": [],
                 "jumps": [], "bpm": []} for c in cases}
    lib = {"kick": [], "wide": [], "lib": []}
    n = 0
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        wons = B.wide_onsets(x)
        orc = O.load(k)
        if orc is None or len(ons) < 4 or len(wons) < 4:
            continue
        lb = np.array(orc["beats"], dtype=float)
        if len(lb) < 4 or orc["bpm"] <= 0.0:
            continue

        # ---- the old tracker, on its own band -----------------------------
        odf_old = device_odf(x)
        if len(odf_old) >= 1200:
            st = S.TempoState()
            beats_f, _peaks = S.firmware_frame_loop(odf_old, st)
            if len(beats_f):
                tb = np.array(beats_f) / S.RATE
                data["old"]["kick"].append(P.hit_with_offset(tb, ons)[0])
                data["old"]["wide"].append(P.hit_with_offset(tb, wons)[0])
                data["old"]["lib"].append(P.hit_with_offset(tb, lb)[0])
                e, lv = rate_err([st.bpm], orc["bpm"])
                data["old"]["err"].append(e)
                data["old"]["bpm"].append(st.bpm)

        # ---- the new tracker, twice ---------------------------------------
        for tag, lo, hi in (("new40", 40.0, 160.0), ("new80", 80.0, 320.0)):
            odf = B.firmware_odf(x, lo, hi)
            if len(odf) < 1200:
                continue
            t = new_tracker(args.margin, args.gain, args.prior)
            t.run(odf)
            if not t.beats:
                continue
            tb = np.array(t.beats) / P.RATE
            data[tag]["kick"].append(P.hit_with_offset(tb, ons)[0])
            data[tag]["wide"].append(P.hit_with_offset(tb, wons)[0])
            data[tag]["lib"].append(P.hit_with_offset(tb, lb)[0])
            e, lv = rate_err([t.bpm], orc["bpm"])
            data[tag]["err"].append(e)
            data[tag]["bpm"].append(t.bpm)

        lib["kick"].append(P.hit_with_offset(lb, ons)[0])
        lib["wide"].append(P.hit_with_offset(lb, wons)[0])
        lib["lib"].append(1.0)
        n += 1
        if n % 20 == 0:
            print("  %d windows  %.0f s" % (n, time.time() - t0), flush=True)

    print("%d windows   %.0f s" % (n, time.time() - t0))
    print()
    print("%-22s | %-19s | %-19s | %-15s | %s"
          % ("", "vs the KICK onsets", "vs the WIDE onsets",
             "vs librosa's beats", "the RATE vs librosa"))
    print("%-22s | %8s %8s | %8s %8s | %8s | %8s %8s"
          % ("", "median", "mean", "median", "mean", "median", "median",
             "mean err"))
    print("-" * 104)
    for c, label in (("old", "OLD, 40-160 Hz"),
                     ("new40", "NEW, 40-160 Hz"),
                     ("new80", "NEW, 80-320 Hz")):
        d = data[c]
        kk = np.array(d["kick"])
        ww = np.array(d["wide"])
        ll = np.array(d["lib"])
        ee = np.array(d["err"])
        print("%-22s | %8.4f %8.4f | %8.4f %8.4f | %8.4f | %8.3f %8.3f"
              % (label, float(np.median(kk)), float(kk.mean()),
                 float(np.median(ww)), float(ww.mean()),
                 float(np.median(ll)), float(np.median(ee)), float(ee.mean())))
    kk = np.array(lib["kick"])
    ww = np.array(lib["wide"])
    print("%-22s | %8.4f %8.4f | %8.4f %8.4f | %8s | %8s %8s"
          % ("librosa's beats", float(np.median(kk)), float(kk.mean()),
             float(np.median(ww)), float(ww.mean()), "-", "-", "-"))
    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

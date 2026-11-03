#!/usr/bin/env python3
"""REVIEW CHECK: does the port's default PLL (the clamp) change the answers?

The port's default pll_gain is 0.0, which selects the CLAMP branch - the
response the firmware deleted.  This measures the same things the margin,
the margin distribution and the band were fitted on, with the clamp and
with the shipped proportional response (0.10), so the difference can be
seen.

    python -u tools/ar_sim/_rev_pllcheck.py --windows 12
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
from _diag_phase import kick_onsets                # noqa: E402

ALL_BPMS = P.BPM_LO + np.arange(P.CAND_COUNT) * P.CAND_STEP


def shipped(gain, margin=0.20):
    """The SHIPPED firmware's configuration (ar_tempo.h), plus a gain knob."""
    t = P.Tracker()
    t.narrow_bpm = 2.0          # AR_TEMPO_NARROW_BPM
    t.search_every = 4          # AR_TEMPO_SEARCH_EVERY
    t.full_every = 32           # AR_TEMPO_FULL_EVERY
    t.collapse_trigger = True
    t.edge_rule = False
    t.net_margin = margin       # AR_TEMPO_NET_MARGIN
    t.pll_gain = gain
    return t


def port_default(gain):
    """The port's DEFAULTS - which are not the shipped firmware."""
    t = P.Tracker()
    t.pll_gain = gain
    return t


def wander(t):
    log = np.array(t.bpm_log)
    half = log[len(log) // 2:]
    half = half[half > 0]
    if len(half) < 100:
        return 0.0, 0, 0.0
    jumps = int(np.sum(np.abs(np.diff(half)) > 0.5))
    biggest = float(np.max(np.abs(np.diff(half)))) if len(half) > 1 else 0.0
    return float(np.std(half)), jumps, biggest


def snapshot(t, frame):
    cutoff = frame - P.COMB_FRAMES
    keep = [(f, s) for (f, s) in t.peak_frame if f >= cutoff]
    if len(keep) < P.MIN_PEAKS:
        return None, None
    return (np.array([f for f, _ in keep], dtype=float),
            np.array([s for _, s in keep], dtype=float))


def margin_at(t, frame, rate):
    frames, strs = snapshot(t, frame)
    if frames is None:
        return None
    scores, _phases = t.comb_scores_all(frames, strs, ALL_BPMS)
    values = scores - t.prior
    jg = int(np.argmax(values))
    lo = max(P.BPM_LO, rate - 2.0)
    hi = min(P.BPM_HI, rate + 2.0)
    mask = (ALL_BPMS >= lo - 1e-6) & (ALL_BPMS <= hi + 1e-6)
    if not mask.any():
        return None
    idx = np.where(mask)[0]
    jn = int(idx[np.argmax(values[idx])])
    return (float(ALL_BPMS[jg]), float(scores[jg]),
            float(ALL_BPMS[jn]), float(scores[jn]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=12)
    args = ap.parse_args()

    t0 = time.time()
    items = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, 80.0, 320.0)     # the shipped band
        if len(odf) < 1200:
            continue
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        if len(ons) < 4:
            continue
        items.append((k, odf, ons))
    print("%d windows   %.0f s" % (len(items), time.time() - t0), flush=True)

    # ---- A: the port's DEFAULT config, clamp vs proportional -------------
    print()
    print("A: the PORT'S DEFAULT config (whole-band search every search)")
    for gain in (0.0, 0.10):
        hits, wand, big = [], [], []
        for (k, odf, ons) in items:
            t = port_default(gain)
            t.run(odf)
            if not t.beats:
                hits.append(0.0)
                wand.append(0.0)
                big.append(0.0)
                continue
            f, _off = P.hit_with_offset(np.array(t.beats) / P.RATE, ons)
            hits.append(f)
            w, _j, b = wander(t)
            wand.append(w)
            big.append(b)
        print("   gain %.2f   hit %.3f   wander %.3f   biggest %.3f"
              % (gain, float(np.median(hits)), float(np.mean(wand)),
                 float(np.mean(big))), flush=True)

    # ---- B: the SHIPPED config, clamp vs proportional --------------------
    print()
    print("B: the SHIPPED config (narrow 2 BPM, net every 32, veto 0.20)")
    for gain in (0.0, 0.10):
        hits, wand, jumps, big, veto = [], [], [], [], []
        for (k, odf, ons) in items:
            t = shipped(gain)
            t.run(odf)
            if not t.beats:
                hits.append(0.0)
                wand.append(0.0)
                jumps.append(0)
                big.append(0.0)
                veto.append(0)
                continue
            f, _off = P.hit_with_offset(np.array(t.beats) / P.RATE, ons)
            hits.append(f)
            w, j, b = wander(t)
            wand.append(w)
            jumps.append(j)
            big.append(b)
            veto.append(t.net_vetoes)
        print("   gain %.2f   hit %.3f   wander %.3f   jumps %.1f   biggest %.3f"
              "   vetoed %.1f"
              % (gain, float(np.median(hits)), float(np.mean(wand)),
                 float(np.mean(jumps)), float(np.mean(big)),
                 float(np.mean(veto))), flush=True)

    # ---- C: the margin sweep, clamp vs proportional ----------------------
    print()
    print("C: the MARGIN sweep, on the shipped config")
    for gain in (0.0, 0.10):
        for m in (0.0, 0.10, 0.20, 0.30):
            hits, wand, veto = [], [], []
            for (k, odf, ons) in items:
                t = shipped(gain, margin=m)
                t.run(odf)
                if not t.beats:
                    hits.append(0.0)
                    wand.append(0.0)
                    veto.append(0)
                    continue
                f, _off = P.hit_with_offset(np.array(t.beats) / P.RATE, ons)
                hits.append(f)
                w, _j, _b = wander(t)
                wand.append(w)
                veto.append(t.net_vetoes)
            print("   gain %.2f  margin %.2f   hit %.3f   wander %.3f   vetoed %.1f"
                  % (gain, m, float(np.median(hits)), float(np.mean(wand)),
                     float(np.mean(veto))), flush=True)
        print()

    # ---- D: the margin distribution, clamp vs proportional ---------------
    print("D: the MARGIN DISTRIBUTION (the ratio the firmware quotes)")
    for gain in (0.0, 0.10):
        same, changed = [], []
        for (k, odf, ons) in items:
            t = shipped(gain)
            t.run(odf)
            if not t.locked or t.bpm <= 0:
                continue
            m = margin_at(t, t.frame_index, t.bpm)
            if m is None:
                continue
            gb, gs, nb, ns = m
            if abs(gb - nb) > 2.0:
                same.append(gs / max(ns, 1e-9))
        for i in range(len(items) - 1):
            for j in range(i + 1, len(items)):
                odf_a, odf_b = items[i][1], items[j][1]
                t = shipped(gain)
                t.run(np.concatenate((odf_a, odf_b)))
                cut = len(odf_a)
                log = np.array(t.bpm_log)
                pre = log[:cut]
                pre = pre[pre > 0]
                if len(pre) < 50:
                    continue
                rate = float(pre[-1])
                m = margin_at(t, cut, rate)
                if m is None:
                    continue
                gb, gs, nb, ns = m
                if abs(gb - rate) > 2.0:
                    changed.append(gs / max(ns, 1e-9))
                break
        for name, arr in (("same ", same), ("chg  ", changed)):
            if arr:
                a = np.array(arr)
                print("   gain %.2f  %s n=%2d  min %.3f  p10 %.3f  med %.3f"
                      "  p90 %.3f  max %.3f"
                      % (gain, name, len(a), a.min(),
                         float(np.percentile(a, 10)), float(np.median(a)),
                         float(np.percentile(a, 90)), a.max()), flush=True)
        print()

    # ---- E: the band, on the shipped config, clamp vs proportional -------
    print("E: the BAND, on the shipped config")
    for gain in (0.0, 0.10):
        for (lo, hi) in ((40.0, 160.0), (60.0, 240.0), (80.0, 320.0),
                         (80.0, 240.0)):
            hits = []
            for (k, odf, ons) in items:
                x = _cache.load(k)
                o2 = B.firmware_odf(x, lo, hi)
                t = shipped(gain)
                t.run(o2)
                if not t.beats:
                    hits.append(0.0)
                    continue
                f, _off = P.hit_with_offset(np.array(t.beats) / P.RATE, ons)
                hits.append(f)
            a = np.array(hits)
            print("   gain %.2f  band %3g-%3g   hit med %.4f   mean %.4f"
                  "   >=0.5 %5.1f%%"
                  % (gain, lo, hi, float(np.median(a)), float(a.mean()),
                     100.0 * float((a >= 0.5).mean())), flush=True)
        print()

    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

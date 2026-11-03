#!/usr/bin/env python3
"""Experiment 16: the last open questions, measured.

  1. does the 3 kHz decimated front end give the same answer as the 48 kHz
     one?   (the earlier --decimated test compared two DIFFERENT things: it
     built a per-sample ODF at 3 kHz and then analysed it as if it were at
     93.75 Hz, and it reused one impulse response for three different
     cascades)
  2. two sections or three?  the front end is the whole per-frame cost.
  3. which tempi break the band 90-176?
  4. which way should the 3:2 tie-break go?
  5. what separates a right answer from a wrong one?  (the confidence)
  6. can the downbeat be found from the audio at all?
  7. how sharp is the onset, really?  (the sub-frame timing)

    python tools/ar_sim/_tempo16_final.py --all
"""

import argparse
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _tempo3_sim as T3                                    # noqa: E402
import _tempo4_sim as T4                                    # noqa: E402
from _tempo2_sim import STYLES, make_track, _seed           # noqa: E402

SR = T3.SR
HOP = T3.HOP
RATE = T3.RATE
NFFT = 4096
LEN = 1125                     # 12.0 s of frames


# --------------------------------------------------------------------------
# The ODF, three ways.  All three produce ONE value per 10.67 ms frame, so
# the DFT that follows sees the same rate in every case.
# --------------------------------------------------------------------------

def odf_48k(x, design="hp40_lp160x2"):
    """The 48 kHz path: band-pass, then per-frame mean square, in dB."""
    return T4.odf_fast(x, T3.filter_designs()[design])


def odf_3k(x, dec=16):
    """The cheap path: anti-alias at 48 kHz, decimate to 3 kHz, band-pass
    THERE, then per-frame mean square, in dB.

    The frame is 32 samples at 3 kHz - the same 10.67 ms as 512 samples at
    48 kHz - so the ODF rate is 93.75 Hz either way.
    """
    fs3 = SR / dec
    aa = [T3.rbj_lpf(400.0, SR, T3.BUTTER_Q),
          T3.rbj_lpf(400.0, SR, T3.BUTTER_Q)]
    y = T4.apply_fast(x, aa)
    n = (len(y) // dec) * dec
    y = y[:n].reshape(-1, dec).mean(axis=1)          # 3 kHz
    y = T4.apply_fast(y, [T3.rbj_hpf(40.0, fs3, T3.BUTTER_Q)])
    y = T4.apply_fast(y, [T3.rbj_lpf(160.0, fs3, T3.BUTTER_Q),
                          T3.rbj_lpf(160.0, fs3, T3.BUTTER_Q)])
    frames = y.reshape(-1, HOP // dec)
    p = (frames.astype(np.float64) ** 2).mean(axis=1)
    db = 10.0 * np.log10(p + T3.EPS)
    return np.maximum(np.diff(db, prepend=db[0]), 0.0)


def odf_3k_max(x, dec=16):
    """The cheap path, but the ODF keeps the sharpest sub-frame event.

    The per-sample log difference is computed at 3 kHz and the MAXIMUM over
    each frame is taken: an onset is one sample wide, so the frame's value
    is the onset's own height, not the frame's average.
    """
    fs3 = SR / dec
    aa = [T3.rbj_lpf(400.0, SR, T3.BUTTER_Q),
          T3.rbj_lpf(400.0, SR, T3.BUTTER_Q)]
    y = T4.apply_fast(x, aa)
    n = (len(y) // dec) * dec
    y = y[:n].reshape(-1, dec).mean(axis=1)
    y = T4.apply_fast(y, [T3.rbj_hpf(40.0, fs3, T3.BUTTER_Q)])
    y = T4.apply_fast(y, [T3.rbj_lpf(160.0, fs3, T3.BUTTER_Q),
                          T3.rbj_lpf(160.0, fs3, T3.BUTTER_Q)])
    p = y.astype(np.float64) ** 2
    db = 10.0 * np.log10(p + T3.EPS)
    d = np.maximum(np.diff(db, prepend=db[0]), 0.0)
    k = HOP // dec
    nfr = (len(d) // k) * k
    return d[:nfr].reshape(-1, k).max(axis=1)


# --------------------------------------------------------------------------
# The estimate, from an ODF.
# --------------------------------------------------------------------------

def krange(lo, hi, nfft=NFFT):
    kmin = int(math.floor(lo / 60.0 * nfft / RATE)) - 1
    kmax = int(math.ceil(hi / 60.0 * nfft / RATE)) + 1
    return kmin, kmax


def estimate(odf, lo=90.0, hi=176.0, tie="32", nfft=NFFT, length=LEN):
    kmin, kmax = krange(lo, hi, nfft)
    mags, _ = T4.running_dft_bins(odf, nfft, kmin, kmax, length)
    fn = T4.TIES[tie]
    est, mag, kb = fn(mags, kmin, kmax, nfft, lo, hi)
    return est, mag, kb, mags, kmin, kmax


def features(odf, lo=90.0, hi=176.0, tie="32", nfft=NFFT, length=LEN):
    """The confidence features, measured on one ODF."""
    est, mag, kb, mags, kmin, kmax = estimate(odf, lo, hi, tie, nfft, length)
    s0, s1 = T4.band_bins(lo, hi, nfft)
    # the background: the median of the in-band magnitudes, minus a guard
    guard = [k - kmin for k in range(s0, s1 + 1)
             if kb is None or abs(k - kb) > 3]
    bg = float(np.median(mags[guard])) if guard else 0.0
    snr = 20.0 * math.log10((mag + 1e-30) / (bg + 1e-30))
    # the strongest competitor that is NOT a simple relative of the winner
    rel = ((2, 1), (3, 1), (4, 1), (1, 2), (1, 3), (1, 4), (3, 2), (2, 3),
           (4, 3), (3, 4))
    clarity = 99.0
    for pb, pm, _pk in T4.local_peaks(mags, kmin, kmax, nfft, lo, hi):
        if est is None:
            break
        r = max(est / pb, pb / est)
        if any(abs(r - float(p) / float(q)) < 0.012 * float(p) / float(q)
               for (p, q) in rel):
            continue
        clarity = min(clarity, pm)
    clar = 20.0 * math.log10((mag + 1e-30) / (clarity + 1e-30))
    # the winner at the very edge of the band means the truth may be outside
    edge = 0
    if kb is not None:
        edge = 1 if (kb <= s0 + 1 or kb >= s1 - 1) else 0
    # the half-window agreement
    half = length // 2
    o2 = odf[len(odf) - half:]
    e2, m2, kb2, _, _, _ = estimate(o2, lo, hi, tie, nfft, half)
    agree = 1 if (e2 is not None and est is not None
                  and abs(e2 - est) < 2.0) else 0
    return dict(est=est, snr=snr, clar=clar, edge=edge, agree=agree)


# --------------------------------------------------------------------------
# 1 + 2: the front end.
# --------------------------------------------------------------------------

def front_end():
    print("=== the front end: 48 kHz vs 3 kHz, and 2 sections vs 3 ===")
    bands = [(90, 176), (120, 176), (90, 150)]
    for lo, hi in bands:
        print("\n--- band %d-%d ---" % (lo, hi))
        print("%-9s %6s %9s %9s %9s %9s" %
              ("style", "bpm", "48k_x2", "48k_x3", "3k", "3k_max"))
        agree48 = agree3 = 0
        tot = 0
        for style in STYLES:
            for bpm in T4.BPMS:
                if not (lo <= bpm <= hi):
                    continue
                x = make_track(bpm, style=style, seed=_seed(style, bpm))
                e2, _, _ = estimate(odf_48k(x, "hp40_lp160"), lo, hi)[:3]
                e3, _, _ = estimate(odf_48k(x, "hp40_lp160x2"), lo, hi)[:3]
                e4, _, _ = estimate(odf_3k(x), lo, hi)[:3]
                e5, _, _ = estimate(odf_3k_max(x), lo, hi)[:3]
                tot += 1
                ok2 = abs(e2 - bpm) < 2.0
                ok3 = abs(e3 - bpm) < 2.0
                ok4 = abs(e4 - bpm) < 2.0
                ok5 = abs(e5 - bpm) < 2.0
                agree48 += 1 if ok2 == ok3 else 0
                agree3 += 1 if ok4 == ok3 else 0
                print("%-9s %6d %9s %9s %9s %9s" %
                      (style, bpm,
                       ("%.2f%s" % (e2, "" if ok2 else "!")),
                       ("%.2f%s" % (e3, "" if ok3 else "!")),
                       ("%.2f%s" % (e4, "" if ok4 else "!")),
                       ("%.2f%s" % (e5, "" if ok5 else "!"))))
        print("2-section vs 3-section agree on %d of %d;  3 kHz vs 48 kHz "
              "agree on %d of %d" % (agree48, tot, agree3, tot))
    return 0


# --------------------------------------------------------------------------
# 3: the wide grid.
# --------------------------------------------------------------------------

def wide():
    print("=== the wide grid: which tempi break the band ===")
    for lo, hi in [(90, 176), (90, 175), (90, 150), (120, 176), (80, 176)]:
        print("\n--- band %d-%d, tie 32 ---" % (lo, hi))
        bad = []
        for style in STYLES:
            for bpm in T4.BPMS_WIDE:
                x = make_track(bpm, style=style, seed=_seed(style, bpm))
                est, _, _ = estimate(odf_48k(x), lo, hi)[:3]
                k = T4.classify(est, bpm)
                if k != "ok":
                    bad.append((style, bpm, est, k))
                    print("  %-9s %6d -> %8.2f  %s" % (style, bpm, est, k))
        print("  %d wrong of %d" % (len(bad), len(STYLES) * len(T4.BPMS_WIDE)))
    return 0


# --------------------------------------------------------------------------
# 4: which way does the 3:2 tie-break go?
# --------------------------------------------------------------------------

def tie_way():
    print("=== the 3:2 tie-break: demote to the slower, or promote to the "
          "faster? ===")
    for lo, hi in [(90, 176), (90, 175), (120, 176)]:
        for tie in ("none", "32", "32up"):
            wrong = 0
            tot = 0
            kinds = {}
            for style in STYLES:
                for bpm in T4.BPMS:
                    if not (lo <= bpm <= hi):
                        continue
                    tot += 1
                    est, _, _ = estimate(T4.odf_for(style, bpm), lo, hi,
                                         tie)[:3]
                    k = T4.classify(est, bpm)
                    if k != "ok":
                        wrong += 1
                        kinds[k] = kinds.get(k, 0) + 1
            print("band %d-%d  tie %-5s  wrong %d of %d  %s"
                  % (lo, hi, tie, wrong, tot, kinds))
    return 0


# --------------------------------------------------------------------------
# 5: the confidence.
# --------------------------------------------------------------------------

def confidence():
    print("=== the confidence: what separates right from wrong ===")
    # the wrong cases are made on purpose: the truth is outside the band
    cases = []
    for lo, hi in [(90, 176), (120, 176), (90, 150)]:
        for style in STYLES:
            for bpm in T4.BPMS:
                odf = T4.odf_for(style, bpm)
                f = features(odf, lo, hi)
                ok = T4.classify(f["est"], bpm) == "ok"
                cases.append((lo, hi, style, bpm, ok, f))
    good = [c for c in cases if c[4]]
    bad = [c for c in cases if not c[4]]
    print("correct %d, wrong %d" % (len(good), len(bad)))
    for name, key in (("snr", "snr"), ("clarity", "clar"), ("agree", "agree"),
                      ("edge", "edge")):
        gv = [c[5][key] for c in good]
        bv = [c[5][key] for c in bad]
        print("%-8s correct %8.2f..%8.2f (med %8.2f)   wrong %8.2f..%8.2f "
              "(med %8.2f)" %
              (name, min(gv), max(gv), float(np.median(gv)),
               min(bv) if bv else 0, max(bv) if bv else 0,
               float(np.median(bv)) if bv else 0))
    print("\nthe wrong cases, one per line:")
    print("%-9s %-9s %6s %9s %8s %8s %6s %6s" %
          ("band", "style", "bpm", "est", "snr", "clar", "agree", "edge"))
    for lo, hi, style, bpm, ok, f in bad:
        print("%-9s %-9s %6d %9.2f %8.2f %8.2f %6d %6d" %
              ("%d-%d" % (lo, hi), style, bpm, f["est"], f["snr"], f["clar"],
               f["agree"], f["edge"]))
    # the combined rule
    print("\nthe combined rule, over all %d cases:" % len(cases))
    for thr in (6.0, 9.0, 12.0, 15.0, 18.0):
        tp = fp = fn = tn = 0
        for lo, hi, style, bpm, ok, f in cases:
            conf = (f["agree"] == 1) and (f["snr"] >= thr)
            if ok and conf:
                tp += 1
            elif ok and not conf:
                fn += 1
            elif not ok and conf:
                fp += 1
            else:
                tn += 1
        print("  snr >= %4.1f dB and the halves agree : "
              "right %3d kept, %3d dropped, %3d wrong kept, %3d wrong dropped"
              % (thr, tp, fn, fp, tn))
    return 0


# --------------------------------------------------------------------------
# 6: the downbeat.
# --------------------------------------------------------------------------

def downbeat():
    """The downbeat, from the audio alone.

    Once the beat period T and the beat phase are known, the beat-synchronous
    series (the ODF sampled AT the beats) is split into four residues.  The
    4-point DFT of that series says which residue carries the bar: the
    residue whose sum is the largest is the downbeat.

    The question is not whether this works but for WHICH music - a
    four-on-the-floor kick has no audible downbeat at all, so the best the
    detector can do there is a coin toss.
    """
    print("=== the downbeat: the 4-point DFT of the beat-synchronous series "
          "===")
    hit = 0
    tot = 0
    per_style = {}
    for style in STYLES:
        sh = 0
        st = 0
        for bpm in (100, 120, 128, 140, 150):
            x = make_track(bpm, style=style, seed=_seed(style, bpm))
            odf = odf_48k(x)
            est, _, _ = estimate(odf, 90, 176)[:3]
            if est is None:
                continue
            T = 60.0 / est * RATE          # the beat period, in frames
            # the beat phase: the strongest ODF sample of the last 4 s
            tail = odf[-int(4 * RATE):]
            phase = len(odf) - int(4 * RATE) + int(np.argmax(tail))
            # the beat-synchronous series
            nb = int((len(odf) - phase) / T)
            s = []
            for b in range(max(0, nb - 32), nb):
                idx = int(round(phase + b * T))
                if 0 <= idx < len(odf):
                    s.append(odf[idx])
            if len(s) < 8:
                continue
            s = np.array(s)
            # the four residues
            idx = np.arange(len(s)) % 4
            sums = [float(np.sum(s[idx == c])) for c in range(4)]
            best = int(np.argmax(sums))
            st += 1
            # the truth, expressed in the grid's own numbering: the residue
            # of the beat that lands on the generator's downbeat (the beat
            # with inbar == 0).
            t0 = phase / RATE                      # the grid's first beat
            beat_sec = 60.0 / bpm
            bd = int(round(round(t0 / (4.0 * beat_sec)) * 4.0 * beat_sec
                           - t0) / beat_sec)      # the beat index of it
            truth = ((bd % 4) + 4) % 4
            if best == truth:
                sh += 1
                hit += 1
            tot += 1
        per_style[style] = (sh, st)
    for s, (sh, st) in per_style.items():
        print("  %-9s %d of %d" % (s, sh, st))
    print("  total %d of %d" % (hit, tot))
    return 0


# --------------------------------------------------------------------------
# 7: the onset sharpness.
# --------------------------------------------------------------------------

def onset_sharp():
    """How sharp is the onset, and how well can the beat phase be found?

    Three candidates, in order of cost:

      A. the ODF's own peak          - one value every 10.67 ms
      B. the onset inside the frame  - one value every 20.8 us
      C. the running DFT's complex phase at the winning bin

    C costs nothing extra: the accumulators are complex already.
    """
    print("=== the onset: where in the frame is it? ===")
    rows = {k: [] for k in ("A", "B", "C")}
    rowsj = {k: [] for k in ("A", "B", "C")}
    for style in STYLES:
        for bpm in (100, 120, 140, 150):
            x = make_track(bpm, style=style, seed=_seed(style, bpm))
            sections = T3.filter_designs()["hp40_lp160x2"]
            y = T4.apply_fast(x, sections)
            n = (len(y) // HOP) * HOP
            frames = y[:n].reshape(-1, HOP)
            p = (frames.astype(np.float64) ** 2).mean(axis=1)
            db = 10.0 * np.log10(p + T3.EPS)
            odf = np.maximum(np.diff(db, prepend=db[0]), 0.0)
            est, mag, kb, mags, kmin, kmax = estimate(odf, 90, 176)
            if est is None:
                continue
            T = 60.0 / est * RATE            # the beat period, in frames
            # C: the phase of the winning bin, at the interpolated frequency
            _, X = T4.running_dft_bins(odf, NFFT, kmin, kmax, LEN)
            xk = X[kb - kmin]
            fhat = est / 60.0                 # Hz
            pC = (-np.angle(xk) / (2.0 * math.pi)) / fhat * RATE  # in frames
            beat = 60.0 / bpm
            for b in range(2, int(16.0 / beat) - 1):
                t_s = b * beat                # SECONDS
                fi = int(t_s * SR // HOP)
                if fi >= len(odf) or fi < 3:
                    continue
                # A: the ODF peak nearest the beat
                k = int(np.argmax(odf[fi - 3:fi + 4])) + fi - 3
                eA = ((k * HOP + HOP / 2.0) / SR - t_s) * 1000.0
                # B: the biggest per-sample jump inside that frame
                fr = frames[fi]
                db2 = 10.0 * np.log10(fr.astype(np.float64) ** 2 + T3.EPS)
                d2 = np.diff(db2, prepend=db2[0])
                j = int(np.argmax(d2))
                eB = ((fi * HOP + j) / SR - t_s) * 1000.0
                # C: the DFT phase, wrapped into one beat period
                pCframes = pC % T
                pCsec = pCframes / RATE
                eC = (pCsec - t_s) * 1000.0
                # wrap into (-T/2, T/2] frames
                Tsec = T / RATE
                for key, e in (("A", eA), ("B", eB), ("C", eC)):
                    w = ((e / 1000.0 + Tsec / 2.0) % Tsec) - Tsec / 2.0
                    rows[key].append(w * 1000.0)
                rowsj["C"].append(0.0)
            # the jitter of C, after removing a constant bias: the spread of
            # the wrapped error within one track
            if rows["C"]:
                med = float(np.median(rows["C"][-20:]))
                for v in rows["C"][-20:]:
                    rowsj["C"].append(v - med)
    for key, name in (("A", "A the ODF's own peak   "),
                      ("B", "B the onset in the frame "),
                      ("C", "C the DFT's phase       ")):
        v = np.array(rows[key])
        print("%s : median %8.2f ms   90th %8.2f ms   spread(68%%) %7.2f ms"
              % (name, float(np.median(np.abs(v))),
                 float(np.percentile(np.abs(v), 90)),
                 float(np.percentile(np.abs(v), 75) -
                       np.percentile(np.abs(v), 25))))
    print("the frame period is %.2f ms" % (HOP / SR * 1000.0))
    return 0


def lock_and_acf():
    """How long does the estimator take to lock, and how much worse is the
    autocorrelation's quantisation than the DFT's?

    The autocorrelation's tempo is quantised by its lag step: at 128 BPM one
    lag step is 2.9 BPM.  The DFT's grid is 1.373 BPM a bin, and the
    parabolic refinement takes it below that.  This measures both on the
    same ODFs.
    """
    print("=== the lock time, and the DFT against the autocorrelation ===")
    locks = []
    dft_err = []
    acf_err = []
    for style in STYLES:
        for bpm in T4.BPMS:
            odf = T4.odf_for(style, bpm)
            lo, hi = 90.0, 176.0
            kmin, kmax = krange(lo, hi)
            # the estimate, frame by frame, over the whole track
            first = None
            series = []
            for t in range(LEN, len(odf) + 1):
                mags, _ = T4.running_dft_bins(odf[:t], NFFT, kmin, kmax, LEN)
                est, mag, kb = T4.TIES["32"](mags, kmin, kmax, NFFT, lo, hi)
                series.append(est)
                if first is None and est is not None \
                        and abs(est - bpm) < 2.0:
                    first = t
            # the lock time: the first correct frame, and the point from
            # which it is correct for the next two seconds
            good = [1 if (e is not None and abs(e - bpm) < 2.0) else 0
                    for e in series]
            lock = None
            for i in range(len(good) - 200):
                if all(good[i:i + 200]):
                    lock = i
                    break
            if lock is not None:
                locks.append((lock + LEN) / RATE)
            # the two estimates on the FULL window
            mags, _ = T4.running_dft_bins(odf, NFFT, kmin, kmax, LEN)
            e1, _, _ = T4.TIES["32"](mags, kmin, kmax, NFFT, lo, hi)
            dft_err.append(abs(e1 - bpm))
            # the autocorrelation, on the same ODF
            x = odf[-LEN:] - float(np.mean(odf[-LEN:]))
            ac = np.correlate(x, x, mode="full")[LEN - 1:]
            ac = ac / (ac[0] + 1e-30)
            lo_l = int(math.ceil(60.0 * RATE / hi))
            hi_l = int(math.floor(60.0 * RATE / lo))
            seg = ac[lo_l:hi_l + 1]
            k = int(np.argmax(seg)) + lo_l
            # the parabolic refinement, the same as the DFT's
            if 0 < k < len(ac) - 1:
                a_ = math.log(max(ac[k - 1], 1e-30))
                b_ = math.log(max(ac[k], 1e-30))
                c_ = math.log(max(ac[k + 1], 1e-30))
                den = a_ - 2.0 * b_ + c_
                d = 0.5 * (a_ - c_) / den if abs(den) > 1e-12 else 0.0
            else:
                d = 0.0
            if abs(d) < 1.0:
                d = 0.0
            e2 = 60.0 * RATE / (k + d)
            acf_err.append(abs(e2 - bpm))
            print("%-9s %6d  lock %5.1f s   dft %8.2f (%+.2f)   acf %8.2f (%+.2f)"
                  % (style, bpm, (lock + LEN) / RATE if lock is not None
                     else -1.0, e1, e1 - bpm, e2, e2 - bpm))
    print("\nlock time: median %.1f s   90th %.1f s   (from the first frame)"
          % (float(np.median(locks)), float(np.percentile(locks, 90))))
    print("DFT error: median %.3f BPM   worst %.3f BPM"
          % (float(np.median(dft_err)), max(dft_err)))
    print("ACF error: median %.3f BPM   worst %.3f BPM"
          % (float(np.median(acf_err)), max(acf_err)))
    return 0


def window_sweep():
    """How long must the window be, and how long until it locks?

    The window is the estimator's memory.  It sets both the frequency
    resolution (60/W BPM) and the time before the first answer (W seconds).
    """
    print("=== the window: accuracy against the wait ===")
    print("%6s %8s %10s %10s %8s %8s" %
          ("W(s)", "frames", "wrong", "med err", "worst", "lock(s)"))
    for W in (2.0, 3.0, 4.0, 6.0, 8.0, 12.0):
        length = int(W * RATE)
        wrong = 0
        errs = []
        locks = []
        for style in STYLES:
            for bpm in T4.BPMS:
                odf = T4.odf_for(style, bpm)
                kmin, kmax = krange(90.0, 176.0)
                first = None
                series = []
                for t in range(length, len(odf) + 1, 3):
                    mags, _ = T4.running_dft_bins(odf[:t], NFFT, kmin, kmax,
                                                 length)
                    est, mag, kb = T4.TIES["32"](mags, kmin, kmax, NFFT,
                                                 90.0, 176.0)
                    series.append((t, est))
                good = [1 if (e is not None and abs(e - bpm) < 2.0) else 0
                        for (t, e) in series]
                lock = None
                for i in range(len(good) - 60):
                    if all(good[i:i + 60]):
                        lock = series[i][0] / RATE
                        break
                if lock is not None:
                    locks.append(lock)
                t, e = series[-1]
                if e is None or abs(e - bpm) >= 2.0:
                    wrong += 1
                    if W <= 8.0:
                        print("      W=%4.1f  %-9s %6d -> %8.2f  %s"
                              % (W, style, bpm, e if e else -1,
                                 T4.classify(e, bpm)))
                errs.append(abs(e - bpm) if e is not None else 999.0)
        print("%6.1f %8d %10d %10.3f %8.3f %8.1f" %
              (W, length, wrong, float(np.median(errs)), max(errs),
               float(np.median(locks)) if locks else -1.0))
    return 0


def tiewide():
    """The candidate ranges, on the wide grid, at two window lengths."""
    print("=== the candidate ranges on the wide grid (132 tracks) ===")
    for lo, hi in ((90, 176), (80, 176), (70, 176), (70, 180), (90, 150),
                   (120, 176), (100, 175)):
        print("\n--- band %d-%d ---" % (lo, hi))
        for W in (12.0, 6.0):
            length = int(W * RATE)
            for tie in ("none", "32"):
                wrong = 0
                tot = 0
                kinds = {}
                bad = []
                for style in STYLES:
                    for bpm in T4.BPMS_WIDE:
                        odf = T4.odf_for(style, bpm)
                        kmin, kmax = krange(float(lo), float(hi))
                        mags, _ = T4.running_dft_bins(odf, NFFT, kmin, kmax,
                                                     length)
                        est, mag, kb = T4.TIES[tie](mags, kmin, kmax, NFFT,
                                                   float(lo), float(hi))
                        tot += 1
                        k = T4.classify(est, bpm)
                        if k != "ok":
                            wrong += 1
                            kinds[k] = kinds.get(k, 0) + 1
                            bad.append("%s %d->%.1f" % (style, bpm, est))
                print("  W=%4.1f tie %-5s wrong %3d of %d  %s"
                      % (W, tie, wrong, tot, kinds))
                if bad and len(bad) <= 16:
                    print("        %s" % ", ".join(bad))
    return 0


def outofband():
    """Can the estimator know that the truth is BELOW the band?

    The one failure the wide grid has is 88 BPM reading as 176 - the double.
    88 is below the floor of 90, so the answer is not wrong, it is CLIPPED:
    the band cannot express it.  The cure is not a better estimate but an
    admission - and the admission needs the DFT to look BELOW the band.
    """
    print("=== out of band: the winner at the top edge, and the bin at "
          "half of it ===")
    kmin, kmax = krange(90.0, 176.0)
    print("%-9s %6s %9s %8s %8s %8s %8s" %
          ("style", "bpm", "est", "edge", "half_bpm", "half_dB", "snr"))
    for bpm in (88, 90, 92, 100, 120, 150, 174, 176):
        for style in ("house", "garage"):
            odf = T4.odf_for(style, bpm)
            mags, _ = T4.running_dft_bins(odf, NFFT, kmin, kmax, LEN)
            est, mag, kb = T4.TIES["32"](mags, kmin, kmax, NFFT, 90.0, 176.0)
            if est is None:
                continue
            s0, s1 = T4.band_bins(90.0, 176.0, NFFT)
            edge = 1 if (kb <= s0 + 1 or kb >= s1 - 1) else 0
            # the bin at half the winner - it may be BELOW the band
            khalf = int(round(est / 2.0 / 60.0 * NFFT / RATE))
            kh = khalf - kmin
            if 0 <= kh < len(mags):
                halfmag = float(mags[kh])
                halfbpm = T4.bin_bpm(khalf, NFFT)
                halfdb = 20.0 * math.log10((halfmag + 1e-30) /
                                           (mag + 1e-30))
            else:
                halfbpm = halfmag = 0.0
                halfdb = -99.0
            guard = [k - kmin for k in range(s0, s1 + 1)
                     if kb is None or abs(k - kb) > 3]
            bg = float(np.median(mags[guard])) if guard else 0.0
            snr = 20.0 * math.log10((mag + 1e-30) / (bg + 1e-30))
            print("%-9s %6d %9.2f %8d %8.2f %8.2f %8.2f"
                  % (style, bpm, est, edge, halfbpm, halfdb, snr))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--front", action="store_true")
    ap.add_argument("--wide", action="store_true")
    ap.add_argument("--tie", action="store_true")
    ap.add_argument("--conf", action="store_true")
    ap.add_argument("--down", action="store_true")
    ap.add_argument("--sharp", action="store_true")
    ap.add_argument("--lock", action="store_true")
    ap.add_argument("--window", action="store_true")
    ap.add_argument("--tiewide", action="store_true")
    ap.add_argument("--oob", action="store_true")
    a = ap.parse_args()
    if a.all or a.front:
        front_end()
    if a.all or a.wide:
        wide()
    if a.all or a.tie:
        tie_way()
    if a.all or a.conf:
        confidence()
    if a.all or a.down:
        downbeat()
    if a.all or a.sharp:
        onset_sharp()
    if a.all or a.lock:
        lock_and_acf()
    if a.all or a.window:
        window_sweep()
    if a.all or a.tiewide:
        tiewide()
    if a.all or a.oob:
        outofband()
    return 0


if __name__ == "__main__":
    sys.exit(main())

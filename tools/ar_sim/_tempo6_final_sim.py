#!/usr/bin/env python3
"""The final measurement set for the tempo design.

Q3  Which confidence number separates right from wrong?
Q4  How long does the estimate take to LOCK, and does it hold?
Q5  Robustness: jitter, blanked beats, noise - over several band sets.
Q6  The phase detector's beat-time error (short window, exact frequency).
Q7  A FAIR autocorrelation: Davies-style comb-filtered ACF with a log-normal
    prior, which is the strongest classical form of the route the brief
    suggested.  If it loses to the DFT here, the deviation is justified.
Q8  The tempo BAND: coverage, the edge guard, and the rational rivals.
Q9  The downbeat: can the bar's origin be recovered from audio at all?
Q10 The window length: accuracy against cost.

Run: python tools/ar_sim/_tempo6_final_sim.py [q3|q4|q5|q6|q7|q8|q9|q10|all]
"""

import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _tempo2_sim import STYLES, _seed, make_track   # noqa: E402
from _tempo5_lock_sim import (SR, HOP, RATE, ISO, EDGE_LO, EDGE_HI, LEN,   # noqa: E402
                               TRUTHS, track_bands, odf_of, _parabolic,
                               bin_range, tie_break, estimate_spectral)

WINDOW_S = 12.0
NFFT = 4096
BPM_LO, BPM_HI = 90.0, 175.0
KICK = list(range(1, 9))          # 25.12 Hz .. 125.89 Hz


def spec_of(odf, nfft=NFFT):
    x = odf[-LEN:].astype(np.float64)
    x = x - x.mean()
    return np.fft.rfft(x, nfft)


def peaks_of(odf, band=(BPM_LO, BPM_HI), nfft=NFFT):
    X = spec_of(odf, nfft)
    k_min, k_max = bin_range(band[0], band[1], nfft)
    mags = np.abs(X[k_min:k_max + 1])
    bpms = np.arange(k_min, k_max + 1) * RATE / nfft * 60.0
    keep = (bpms >= band[0]) & (bpms <= band[1])
    mags = mags[keep]
    bpms = bpms[keep]
    peaks = []
    for i in range(1, len(mags) - 1):
        if mags[i] >= mags[i - 1] and mags[i] > mags[i + 1]:
            peaks.append((float(bpms[i]), float(mags[i])))
    return peaks, bpms, mags


def spectral(odf, band=(BPM_LO, BPM_HI), tol_db=3.0, ratios=((3, 2),),
             nfft=NFFT):
    """The estimator: the tallest in-band spectral peak, parabolic-refined,
    with the rational tie-break.  Returns (bpm, margin_dB, at_edge)."""
    X = spec_of(odf, nfft)
    k_min, k_max = bin_range(band[0], band[1], nfft)
    mags = np.abs(X[k_min:k_max + 1])
    bpms = np.arange(k_min, k_max + 1) * RATE / nfft * 60.0
    keep = (bpms >= band[0]) & (bpms <= band[1])
    idx = np.where(keep)[0]
    if len(idx) < 3:
        return None, 0.0, False
    sub = mags[idx]
    sb = bpms[idx]
    k = int(np.argmax(sub))
    best = float(sb[k])
    mag = float(sub[k])
    d = _parabolic(np.log(sub + 1e-20), k)
    if d:
        best += d * (RATE / nfft * 60.0)
    edge = bool(k == 0 or k == len(sub) - 1)
    peaks = []
    for i in range(1, len(sub) - 1):
        if sub[i] >= sub[i - 1] and sub[i] > sub[i + 1]:
            peaks.append((float(sb[i]), float(sub[i])))
    best, mag, _ = tie_break(best, mag, peaks, tol_db, ratios)
    # the margin over the nearest NON-rational rival
    runner = 0.0
    for bpm, m in peaks:
        if abs(bpm - best) < 0.5:
            continue
        r = max(best / bpm, bpm / best)
        if any(abs(r - p / q) < 0.012 * (p / q)
               for (p, q) in ((2, 1), (3, 1), (4, 1), (3, 2), (4, 3))):
            continue
        runner = max(runner, m)
    margin = 20.0 * math.log10(mag / runner) if runner > 0 else 30.0
    return best, margin, edge


# --------------------------------------------------------------------------

def q3():
    print("=== Q3: the confidence that separates right from wrong ===")
    ok, bad = [], []
    for style in STYLES:
        for bpm in TRUTHS:
            odf = odf_of(KICK, style, bpm)
            e, margin, edge = spectral(odf)
            right = e is not None and abs(e - bpm) < 2.0
            (ok if right else bad).append(margin)
            print("%-9s %6d  %-6s est %8s  margin %6.2f dB  edge %s" %
                  (style, bpm, "RIGHT" if right else "WRONG",
                   "none" if e is None else "%.1f" % e, margin, edge))
    print()
    print("margin when RIGHT: mean %6.2f dB, min %6.2f dB" %
          (np.mean(ok), np.min(ok)))
    if bad:
        print("margin when WRONG: mean %6.2f dB, max %6.2f dB" %
              (np.mean(bad), np.max(bad)))
    print()
    return


def q4():
    print("=== Q4: the LOCK TIME (the running DFT, frame by frame) ===")
    print("the estimate is right when it is within 2 BPM and STAYS right")
    print()
    print("%-9s %6s %9s %9s" % ("style", "bpm", "lock_s", "final_err"))
    locks = []
    for style in STYLES:
        for bpm in (100, 128, 150):
            odf = odf_of(KICK, style, bpm)
            seq = running_series(odf)
            lock = None
            for n, (fr, e, m) in enumerate(seq):
                if e is not None and abs(e - bpm) < 2.0:
                    if all(s[1] is None or abs(s[1] - bpm) < 2.0 for s in seq[n:]):
                        lock = fr / RATE
                        break
            if lock is None:
                lock = float("nan")
                locks.append(999.0)
            else:
                locks.append(lock)
            print("%-9s %6d %9.1f %9.2f" %
                  (style, bpm, lock, seq[-1][1] - bpm if seq[-1][1] else 0.0))
    print()
    print("lock time: mean %.1f s, worst %.1f s, never %d of %d" %
          (np.mean(locks), np.max(locks),
           sum(1 for v in locks if v > 900), len(locks)))
    print()
    return


def running_series(odf, band=(BPM_LO, BPM_HI), nfft=NFFT):
    """The firmware's estimator, frame by frame: a running DFT over a sliding
    12 s window.  Yields (frame, bpm, margin)."""
    ks = np.arange(bin_range(band[0], band[1], nfft)[0],
                   bin_range(band[0], band[1], nfft)[1] + 1)
    mask = nfft - 1
    W = np.exp(-2j * np.pi * np.arange(nfft) / nfft)
    acc = np.zeros(len(ks), dtype=complex)
    x = odf.astype(np.float64)
    out = []
    for i in range(len(x)):
        idx = (ks * i) & mask
        acc += x[i] * W[idx]
        if i >= LEN:
            idxo = (ks * (i - LEN)) & mask
            acc -= x[i - LEN] * W[idxo]
        if i < LEN - 1:
            continue
        mags = np.abs(acc)
        bpms = ks * RATE / nfft * 60.0
        keep = (bpms >= band[0]) & (bpms <= band[1])
        sub = mags[keep]
        sb = bpms[keep]
        if len(sub) < 3:
            out.append((i, None, 0.0))
            continue
        k = int(np.argmax(sub))
        best = float(sb[k])
        mag = float(sub[k])
        d = _parabolic(np.log(sub + 1e-20), k)
        if d:
            best += d * (RATE / nfft * 60.0)
        peaks = []
        for j in range(1, len(sub) - 1):
            if sub[j] >= sub[j - 1] and sub[j] > sub[j + 1]:
                peaks.append((float(sb[j]), float(sub[j])))
        best, mag, _ = tie_break(best, mag, peaks, 3.0, ((3, 2),))
        out.append((i, best, mag))
    return out


def q5():
    print("=== Q5: robustness over the band sets (band %g-%g) ===" %
          (BPM_LO, BPM_HI))
    sets = [("1-8", list(range(1, 9))), ("2-8", list(range(2, 9))),
            ("3-8", list(range(3, 9))), ("2-9", list(range(2, 10)))]
    cases = [("clean", {}), ("jitter 30ms", {"human": 30.0}),
             ("noise 0.05", {"noise": 0.05})]
    print("%-6s %-12s %6s %9s %8s" % ("bands", "case", "wrong", "maxerr", "rmserr"))
    for name, bands in sets:
        for label, kw in cases:
            errs = []
            wrong = 0
            for style in STYLES:
                for bpm in TRUTHS:
                    odf = odf_of(bands, style, bpm, **kw)
                    e, m, edge = spectral(odf)
                    if e is None:
                        wrong += 1
                        continue
                    err = e - bpm
                    errs.append(err)
                    if abs(err) >= 2.0:
                        wrong += 1
            errs = np.array(errs)
            print("%-6s %-12s %6d %9.2f %8.2f" %
                  (name, label, wrong, np.max(np.abs(errs)),
                   np.sqrt(np.mean(errs ** 2))))
    print()
    print("--- blanked beats (band 1-8, clean) ---")
    for frac in (0.15, 0.30, 0.45):
        errs = []
        wrong = 0
        for style in STYLES:
            for bpm in TRUTHS:
                odf = odf_of(KICK, style, bpm).copy()
                rng = np.random.default_rng(bpm + int(frac * 1000))
                odf[rng.random(len(odf)) < frac] = 0.0
                e, m, edge = spectral(odf)
                if e is None:
                    wrong += 1
                    continue
                err = e - bpm
                errs.append(err)
                if abs(err) >= 2.0:
                    wrong += 1
        errs = np.array(errs)
        print("blank %.0f%%  wrong %2d of %2d  maxerr %7.2f  rms %6.2f" %
              (frac * 100, wrong, len(STYLES) * len(TRUTHS),
               np.max(np.abs(errs)), np.sqrt(np.mean(errs ** 2))))
    print()
    return


def q6():
    print("=== Q6: the phase detector's beat-time error ===")
    print("a short-window complex DFT at the EXACT beat frequency, over the")
    print("free ODF, measured against the generator's own kick times")
    for win_s in (0.5, 1.0, 2.0):
        for bpm in (120.0, 128.0, 140.0, 174.0):
            errs = []
            for style in STYLES:
                odf = odf_of(KICK, style, bpm)
                win = int(win_s * RATE)
                v = odf[-win:].astype(np.float64)
                v = v - v.mean()
                f = bpm / 60.0
                n = np.arange(len(v))
                # the phase relative to the window's CENTRE
                ang = 2.0 * math.pi * f * (n - (len(v) - 1) / 2.0) / RATE
                re = float(np.dot(v, np.cos(ang)))
                im = float(np.dot(v, np.sin(ang)))
                if abs(re) < 1e-20 and abs(im) < 1e-20:
                    continue
                ph = math.atan2(im, re) / (2.0 * math.pi)   # cycles
                period = 60.0 / bpm
                t_end = len(odf) / RATE
                t_centre = t_end - win / (2.0 * RATE)
                # a beat sits at t_centre - ph*period + k*period
                t0 = t_centre - ph * period
                beat = period
                best = None
                for k in range(int(16.0 / beat)):
                    tt = k * beat
                    n2 = round((tt - t0) / period)
                    d = abs(t0 + n2 * period - tt) * 1000.0
                    if best is None or d < best:
                        best = d
                errs.append(best)
            if errs:
                print("  win %4.1f s  %6.1f BPM  mean %6.1f ms  worst %6.1f ms" %
                      (win_s, bpm, np.mean(errs), np.max(errs)))
    print()
    return


def q7():
    print("=== Q7: a FAIR autocorrelation (Davies-style comb + log-normal prior) ===")
    print("This is the strongest classical form of the route the brief asked")
    print("for.  If it loses to the spectral peak here, the deviation is")
    print("justified by measurement rather than by assertion.")
    print()
    print("%-9s %6s %10s %10s %10s %10s" %
          ("style", "bpm", "acf_bpm", "acf_err", "dft_bpm", "dft_err"))
    acf_err = []
    dft_err = []
    for style in STYLES:
        for bpm in TRUTHS:
            odf = odf_of(KICK, style, bpm)
            a = davies_acf(odf)
            d, m, edge = spectral(odf)
            ae = a - bpm
            de = d - bpm
            acf_err.append(ae)
            dft_err.append(de)
            print("%-9s %6d %10.1f %10.2f %10.1f %10.2f" %
                  (style, bpm, a, ae, d, de))
    acf_err = np.array(acf_err)
    dft_err = np.array(dft_err)
    print()
    print("ACF: wrong %d of %d, max err %6.2f, rms %6.2f" %
          (int((np.abs(acf_err) >= 2).sum()), len(acf_err),
           np.max(np.abs(acf_err)), np.sqrt(np.mean(acf_err ** 2))))
    print("DFT: wrong %d of %d, max err %6.2f, rms %6.2f" %
          (int((np.abs(dft_err) >= 2).sum()), len(dft_err),
           np.max(np.abs(dft_err)), np.sqrt(np.mean(dft_err ** 2))))
    print()
    return


def davies_acf(odf, band=(BPM_LO, BPM_HI)):
    """Davies' comb-filtered autocorrelation with a log-normal tempo prior.

    This is the classical 'ACF' route done properly:
      * the overlap-normalised autocorrelation r[L];
      * a log-normal prior over the period, centred on 0.5 s (120 BPM) with
        sigma = 1.4 octaves - Ellis 2007's W(tau);
      * a comb filter over the ACF's own harmonics, which rewards a period
        whose multiples also carry energy (the beat, not the subdivision);
      * the highest scoring period.
    """
    x = odf[-LEN:].astype(np.float64)
    x = x - x.mean()
    n = len(x)
    r = np.correlate(x, x, mode="full")[n - 1:]
    lag_lo = max(2, int(math.floor(60.0 * RATE / band[1])))
    lag_hi = min(n - 3, int(math.ceil(60.0 * RATE / band[0])))
    lags = np.arange(lag_lo, lag_hi + 1)
    # the overlap normalisation
    w = r[lags] / (n - lags)
    # the log-normal prior: theta0 = 0.5 s = 0.5*RATE frames, sigma = 1.4 oct
    theta0 = 0.5 * RATE
    sigma = 1.4
    prior = np.exp(-0.5 * (np.log2(lags / theta0) / sigma) ** 2)
    # the comb: add the ACF at the period's multiples
    score = np.zeros(len(lags))
    for m in range(1, 5):
        idx = np.clip(lags * m - lag_lo, 0, len(w) - 1)
        valid = (lags * m) < len(r)
        s = np.where(valid, w[idx], 0.0)
        score += s / m
    score = score * prior
    k = int(np.argmax(score))
    lag = float(lags[k])
    # parabolic refinement in lag space
    if 0 < k < len(score) - 1:
        d = _parabolic(score, k)
        if d:
            lag += d
    return 60.0 * RATE / lag


def q8():
    print("=== Q8: the tempo BAND ===")
    print("the truths are 90..174; a band that does not CONTAIN the truth")
    print("cannot be right, and the estimate must not pretend otherwise")
    print()
    bands = [(90.0, 150.0), (90.0, 160.0), (90.0, 175.0), (100.0, 175.0),
             (120.0, 176.0), (90.0, 180.0), (80.0, 160.0)]
    print("%-11s %6s %6s %9s %8s %8s" %
          ("band", "inside", "wrong", "maxerr", "rmserr", "at_edge"))
    for band in bands:
        errs = []
        wrong = 0
        inside = 0
        edge = 0
        edge_wrong = 0
        for style in STYLES:
            for bpm in TRUTHS:
                odf = odf_of(KICK, style, bpm)
                e, m, ed = spectral(odf, band)
                if e is None:
                    wrong += 1
                    continue
                if band[0] <= bpm <= band[1]:
                    inside += 1
                    err = e - bpm
                    errs.append(err)
                    if abs(err) >= 2.0:
                        wrong += 1
                        if ed:
                            edge_wrong += 1
                else:
                    # the truth is OUTSIDE: is the winner at the band edge?
                    if ed:
                        edge += 1
                    # and is the answer the nearest plausible in-band value?
                    err = e - bpm
                    errs.append(err)
                    if abs(err) >= 2.0:
                        wrong += 1
        errs = np.array(errs)
        print("%-11s %6d %6d %9.2f %8.2f %8d" %
              ("%g-%g" % band, inside, wrong, np.max(np.abs(errs)),
               np.sqrt(np.mean(errs ** 2)), edge))
    print()
    print("--- the rational rivals inside each band ---")
    for band in bands:
        rivals = set()
        for p in range(1, 5):
            for q in range(1, 5):
                if p == q:
                    continue
                lo, hi = band
                # both p/q*base and q/p*base inside the band for some base
                for base in np.arange(lo, hi, 0.5):
                    for (a, b) in ((p, q), (q, p)):
                        if a > b and lo <= base * a / b <= hi and \
                           lo <= base <= hi:
                            rivals.add((a, b))
        print("%-11s %s" % ("%g-%g" % band, sorted(rivals)))
    print()
    return


def q9():
    print("=== Q9: the downbeat - can the bar's origin be recovered? ===")
    print("the bar phase needs to know WHICH beat is beat 1.  The rule")
    print("tested: the beat position in the bar with the highest mean")
    print("low-band energy is beat 1.")
    print()
    hits = 0
    total = 0
    for style in STYLES:
        for bpm in (120, 128, 140, 150):
            x = make_track(bpm, style=style, seed=_seed(style, bpm))
            # the true downbeat is beat 0 of the bar
            # use the kick band only, and find the strongest of 4 phases
            n = len(x)
            X = np.fft.rfft(x)
            f = np.fft.rfftfreq(n, 1.0 / SR)
            m = ((f >= 25.0) & (f < 130.0)).astype(np.float64)
            y = np.fft.irfft(X * m, n=n)
            n2 = (len(y) // HOP) * HOP
            fr = y[:n2].reshape(-1, HOP)
            p = (fr.astype(np.float64) ** 2).mean(axis=1)
            db = 10.0 * np.log10(p + 1e-12)
            fl = np.maximum(np.diff(db, prepend=db[0]), 0.0)
            beat_frames = 60.0 / bpm * RATE
            # score each of the four bar positions
            scores = np.zeros(4)
            counts = np.zeros(4)
            for i in range(len(fl)):
                ph = int(round(i / beat_frames)) % 4
                scores[ph] += fl[i]
                counts[ph] += 1
            scores = scores / np.maximum(counts, 1)
            best = int(np.argmax(scores))
            hits += 1 if best == 0 else 0
            total += 1
            print("%-9s %4d  chosen bar beat %d  scores %s" %
                  (style, bpm, best,
                   " ".join("%6.3f" % s for s in scores)))
    print()
    print("the downbeat rule got %d of %d right (chance is %d of %d)" %
          (hits, total, total // 4, total))
    print()
    return


def q10():
    print("=== Q10: the window length, against cost ===")
    print("the running DFT's cost per frame is O(bins), independent of the")
    print("window; the window sets the RESOLUTION and the lock time")
    print()
    global LEN
    for win_s in (4.0, 6.0, 8.0, 12.0, 16.0):
        LEN = int(win_s * RATE)
        errs = []
        wrong = 0
        for style in STYLES:
            for bpm in TRUTHS:
                odf = odf_of(KICK, style, bpm)
                e, m, edge = spectral(odf)
                if e is None:
                    wrong += 1
                    continue
                err = e - bpm
                errs.append(err)
                if abs(err) >= 2.0:
                    wrong += 1
        errs = np.array(errs)
        rayleigh = 60.0 / win_s
        print("window %5.1f s  wrong %2d of %2d  maxerr %7.2f  rms %6.2f  "
              "Rayleigh %4.1f BPM" %
              (win_s, wrong, len(STYLES) * len(TRUTHS),
               np.max(np.abs(errs)), np.sqrt(np.mean(errs ** 2)), rayleigh))
    LEN = int(WINDOW_S * RATE)
    print()
    return


if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    if which in ("all", "q3"):
        q3()
    if which in ("all", "q4"):
        q4()
    if which in ("all", "q5"):
        q5()
    if which in ("all", "q6"):
        q6()
    if which in ("all", "q7"):
        q7()
    if which in ("all", "q8"):
        q8()
    if which in ("all", "q9"):
        q9()
    if which in ("all", "q10"):
        q10()

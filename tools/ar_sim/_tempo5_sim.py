#!/usr/bin/env python3
"""The decisive measurement: WHICH RANGE, and does the DP earn its CPU?

_tempo3_sim.py measured the running-DFT estimator: 5 wrong of 48 with a
90-175 band.  Two of those five were NOT algorithm failures - they were
BAND-EDGE LEAKS.  The estimator's candidate set was K_MIN..K_MAX, and those
are the band +- ONE BIN, because the margin bin is needed to interpolate a
peak parabolically.  But the margin bin was also allowed to BE the peak.  So
a 90 BPM track, whose 8th-note grid lives at 180 BPM, had its excluded
octave partner leak back in through the margin bin and win:

    house  90 -> 177.15   (bin 129 = K_MAX = 177.15, i.e. the truncated 180)

The fix is one line of policy: the margin bin may REFINISH a peak, it may
not BE one.  This file measures that fix, sweeps the candidate RANGES, and
compares the spectral estimator against Ellis's DP over onset candidates.

    python tools/ar_sim/_tempo5_sim.py --bands
    python tools/ar_sim/_tempo5_sim.py --bands --dp
    python tools/ar_sim/_tempo5_sim.py --cost
"""

import argparse
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _tempo2_sim import STYLES, inject_skips                        # noqa: E402
from _tempo3_sim import (                                             # noqa: E402
    SR, HOP, RATE, LEN, NFFT, EPS, BUTTER_Q, rbj_hpf, rbj_lpf, rbj_bpf,
    filter_designs, section_ir, twiddle, _seed, make_track,
)

# The grid is widened so every band below can be carved out of it.
GRID_LO_BPM, GRID_HI_BPM = 60.0, 195.0
BPM_PER_BIN = RATE / NFFT * 60.0
GK_MIN = int(math.floor(GRID_LO_BPM / BPM_PER_BIN)) - 2
GK_MAX = int(math.ceil(GRID_HI_BPM / BPM_PER_BIN)) + 2
GK_N = GK_MAX - GK_MIN + 1

DP_ALPHA = 68.0
F1_TOL = 0.070

BANDS = [
    ("90-150", 90.0, 150.0),
    ("90-175", 90.0, 175.0),
    ("90-176", 90.0, 176.0),
    ("100-175", 100.0, 175.0),
    ("120-176", 120.0, 176.0),
    ("118-176", 118.0, 176.0),
    ("70-176", 70.0, 176.0),
]


# --------------------------------------------------------------------------
# The fast ODF: the same filter, but the cascade IR is made with FFTs
# --------------------------------------------------------------------------

def cascade_ir_fast(sections, n=8192):
    ir = np.ones(1)
    for c in sections:
        s = section_ir(c, n)
        m = 1 << int(math.ceil(math.log2(len(ir) + len(s) - 1)))
        ir = np.fft.irfft(np.fft.rfft(ir, m) * np.fft.rfft(s, m), m)[:n]
    return ir


def apply_fast(x, sections):
    if sections is None:
        return x.astype(np.float64)
    ir = cascade_ir_fast(sections)
    n = len(x) + len(ir) - 1
    nfft = 1 << int(math.ceil(math.log2(n)))
    return np.fft.irfft(np.fft.rfft(x, nfft) * np.fft.rfft(ir, nfft), nfft)[:len(x)]


def odf_fast(x, sections):
    y = apply_fast(x, sections)
    n = (len(y) // HOP) * HOP
    frames = y[:n].reshape(-1, HOP)
    p = (frames.astype(np.float64) ** 2).mean(axis=1)
    db = 10.0 * np.log10(p + EPS)
    return np.maximum(np.diff(db, prepend=db[0]), 0.0)


# --------------------------------------------------------------------------
# The running DFT, over the widened grid
# --------------------------------------------------------------------------

def running_dft_grid(odf):
    tw = twiddle()
    ks = np.arange(GK_MIN, GK_MAX + 1, dtype=np.int64)
    acc = np.zeros((GK_N, 2), dtype=np.float64)
    n = len(odf)
    for i in range(n):
        idx = (ks * (i & (NFFT - 1))) & (NFFT - 1)
        f = tw[idx]
        v = float(odf[i])
        acc[:, 0] += v * f[:, 0]
        acc[:, 1] += v * f[:, 1]
        if i >= LEN:
            j = i - LEN
            idxo = (ks * (j & (NFFT - 1))) & (NFFT - 1)
            fo = tw[idxo]
            vo = float(odf[j])
            acc[:, 0] -= vo * fo[:, 0]
            acc[:, 1] -= vo * fo[:, 1]
    return np.sqrt(acc[:, 0] ** 2 + acc[:, 1] ** 2)


def bin_bpm(k):
    return k * BPM_PER_BIN


def band_bins(lo_bpm, hi_bpm):
    """The CANDIDATE bins: the ones whose centre is inside the band.

    The +-1 bin margin around them exists only so a peak at the edge can be
    interpolated parabolically; a margin bin is never a candidate."""
    klo = int(math.ceil(lo_bpm / BPM_PER_BIN))
    khi = int(math.floor(hi_bpm / BPM_PER_BIN))
    klo = max(klo, GK_MIN + 1)
    khi = min(khi, GK_MAX - 1)
    return klo, khi


def peak_strict(mags, klo, khi):
    """The tallest candidate bin, parabolically refined on the log."""
    if khi < klo:
        return None, 0.0
    seg = mags[klo - GK_MIN:khi - GK_MIN + 1]
    kbest = klo + int(np.argmax(seg))
    bpm = bin_bpm(kbest)
    m = float(mags[kbest - GK_MIN])
    if GK_MIN < kbest < GK_MAX:
        a = math.log(mags[kbest - 1 - GK_MIN] + 1e-30)
        b = math.log(m + 1e-30)
        c = math.log(mags[kbest + 1 - GK_MIN] + 1e-30)
        den = a - 2.0 * b + c
        if abs(den) > 1e-12:
            d = 0.5 * (a - c) / den
            if abs(d) < 1.0:
                bpm += d * BPM_PER_BIN
    return bpm, m


def local_peaks_strict(mags, klo, khi):
    out = []
    for k in range(klo, khi + 1):
        m = float(mags[k - GK_MIN])
        if m >= float(mags[k - 1 - GK_MIN]) and m >= float(mags[k + 1 - GK_MIN]):
            out.append((bin_bpm(k), m))
    return out


def rational_slower(winner, wmag, mags, klo, khi, tol_db=3.0, max_ratio=4):
    """The 3:2 / 4:3 tie-break: a LOWER candidate within tol_db whose ratio
    to the winner is a simple p/q wins.

    tol_db <= 0 disables it."""
    if tol_db <= 0.0:
        return winner
    thr = wmag / (10.0 ** (tol_db / 20.0))
    for b, m in local_peaks_strict(mags, klo, khi):
        if b >= winner - 1e-9:
            continue
        if m <= 0.0 or m < thr:
            continue
        r = winner / b
        for q in range(1, max_ratio + 1):
            for p in range(q + 1, max_ratio + 1):
                if abs(r - p / q) < 0.01 * (p / q):
                    return b
    return winner


def spectral_pick(mags, lo_bpm, hi_bpm, tie_db=3.0):
    klo, khi = band_bins(lo_bpm, hi_bpm)
    if khi < klo:
        return None, 0.0, []
    bpm, mag = peak_strict(mags, klo, khi)
    if bpm is None:
        return None, 0.0, []
    bpm = rational_slower(bpm, mag, mags, klo, khi, tie_db)
    cands = [b for b, _ in local_peaks_strict(mags, klo, khi)]
    return bpm, mag, cands


# --------------------------------------------------------------------------
# The onset candidates and the DP
# --------------------------------------------------------------------------

def onset_candidates(odf, thresh_rel=0.10, min_gap_frames=4):
    if len(odf) < 3:
        return np.array([], dtype=np.int64)
    peak = float(np.max(odf))
    if peak <= 0.0:
        return np.array([], dtype=np.int64)
    thr = thresh_rel * peak
    cand = []
    n = len(odf)
    for i in range(1, n - 1):
        v = odf[i]
        if v < thr:
            continue
        if v < odf[i - 1] or v < odf[i + 1]:
            continue
        if cand and (i - cand[-1]) < min_gap_frames:
            if v > odf[cand[-1]]:
                cand[-1] = i
            continue
        cand.append(i)
    return np.array(cand, dtype=np.int64)


def dp_beats(cand, odf, p, alpha=DP_ALPHA):
    """Ellis's DP over the onset candidates.

    C(i) = O(cand[i]) + min over j with p/2 < cand[i]-cand[j] < 2p of
           { C(j) - alpha*(log((cand[i]-cand[j])/p))^2 }

    Returns (beats, transitions, score).  `transitions` is the number of
    (i,j) pairs evaluated - that is the number that becomes CPU time.
    """
    n = len(cand)
    if n < 3:
        return np.array([], dtype=np.int64), 0, -1e30
    lo = p * 0.5
    hi = p * 2.0
    C = np.full(n, -1e30)
    bp = np.full(n, -1, dtype=np.int64)
    C[0] = odf[cand[0]]
    trans = 0
    for i in range(1, n):
        best = -1e30
        bestj = -1
        for j in range(i - 1, -1, -1):
            tau = cand[i] - cand[j]
            if tau > hi:
                break
            if tau < lo:
                continue
            trans += 1
            v = C[j] - alpha * math.log(tau / p) ** 2
            if v > best:
                best = v
                bestj = j
        if bestj >= 0:
            C[i] = odf[cand[i]] + best
            bp[i] = bestj
    end = int(np.argmax(C))
    if C[end] <= -1e29:
        return np.array([], dtype=np.int64), trans, -1e30
    beats = []
    i = end
    while i >= 0:
        beats.append(int(cand[i]))
        i = int(bp[i])
    beats.reverse()
    return np.array(beats, dtype=np.int64), trans, float(C[end])


def dp_pick(cand, odn, periods, prior_tau0=0.5, prior_sigma=1.4,
            alpha=DP_ALPHA, tol_bpm=2.0):
    """The DP over a set of proposed periods, each scored by its global cost
    times Ellis's log-normal prior.  The best wins.

    `odn` is the ODF NORMALISED to [0,1] over the window.  This matters more
    than anything else in the DP: Ellis's transition penalty is
    -alpha*(log(tau/p))^2, and at alpha = 68 a 2p gap costs -33.  If the ODF
    is in dB (0..20) the onset terms are of the same order as the penalty
    terms, so the DP just picks the strongest onsets and the period parameter
    stops meaning anything - measured: the DP at p = 46.55 frames found the
    TRUE 43.95-frame beat grid and still called it 46.55.  Normalised, the
    penalty dominates and the period parameter means what it says.

    `periods` is in FRAMES.  Two more things have to be right:

      * the prior is on the period in SECONDS, not in frames:
            W(p) = exp(-0.5*(log2(p_sec/tau0)/sigma)^2)
        (using frames makes the prior a monotone function of the frame count
        and it then simply picks the fastest period offered.)

      * the score is LENGTH-NORMALISED.  The DP's own cost C[end] is a sum
        along the path, so a path with twice the beats accumulates twice the
        onset energy and the fastest period always wins.  Dividing by the
        number of beats makes the score "how good a beat is this, on
        average", which is what can be compared across periods.
    """
    best = None
    total_trans = 0
    for p in periods:
        beats, trans, score = dp_beats(cand, odn, p, alpha)
        total_trans += trans
        if len(beats) < 3:
            continue
        w = math.exp(-0.5 * (math.log2((p / RATE) / prior_tau0) / prior_sigma) ** 2)
        s = (score / len(beats)) * w
        if best is None or s > best[0]:
            best = (s, p, beats)
    if best is None:
        return None, np.array([], dtype=np.int64), total_trans
    return best[1], best[2], total_trans


def beat_period(beats):
    """The period of a found beat sequence, by a least-squares line through
    the beats: t_k = t0 + k*p.

    This is a SECOND estimate of the tempo, independent of the DP's period
    parameter, and it is the one that should be published: it uses the beat
    TIMES, so it cannot be wrong in the way the parameter can.
    """
    n = len(beats)
    if n < 3:
        return 0.0
    k = np.arange(n, dtype=np.float64)
    t = np.asarray(beats, dtype=np.float64)
    km = k.mean()
    tm = t.mean()
    denom = ((k - km) ** 2).sum()
    if denom <= 0.0:
        return 0.0
    return float(((k - km) * (t - tm)).sum() / denom)


def true_beats(bpm, seconds, rate=RATE):
    p = 60.0 / bpm
    n = int(seconds / p)
    return np.arange(n) * p * rate


def f1_score(est, tru, tol_frames):
    if len(est) == 0 or len(tru) == 0:
        return 0.0
    used = np.zeros(len(tru), dtype=bool)
    tp = 0
    for b in est:
        d = np.abs(tru - b)
        d[used] = 1e9
        k = int(np.argmin(d))
        if d[k] <= tol_frames:
            used[k] = True
            tp += 1
    prec = tp / len(est)
    rec = tp / len(tru)
    return 0.0 if (prec + rec) == 0 else 2 * prec * rec / (prec + rec)


# --------------------------------------------------------------------------
# The sweep
# --------------------------------------------------------------------------

_TRACKS = {}


def get_track(style, bpm, human=0.0, noise=0.0):
    key = (style, bpm, human, noise)
    if key not in _TRACKS:
        _TRACKS[key] = make_track(bpm, style=style, seed=_seed(style, bpm),
                                  human=human, noise=noise)
    return _TRACKS[key]


def build(style, bpm, human=0.0, noise=0.0, design="hp40_lp160x2", skip=0.0):
    x = get_track(style, bpm, human, noise)
    odf = odf_fast(x, filter_designs()[design])
    if skip:
        odf = inject_skips(odf, RATE, skip)
    mags = running_dft_grid(odf)
    cand = onset_candidates(odf)
    return odf, mags, cand


def sweep(bands=BANDS, dp=False, human=0.0, noise=0.0, skip=0.0, verbose=False, tie_db=3.0):
    bpms = (90, 100, 120, 128, 140, 150, 160, 174)
    prepared = {}
    for style in STYLES:
        for bpm in bpms:
            prepared[(style, bpm)] = build(style, bpm, human, noise, skip=skip)

    print("%-9s %-9s %7s %7s %7s %9s %9s" %
          ("band", "method", "wrong", "clip", "meanerr", "meanF1", "trans"))
    results = {}
    for name, lo, hi in bands:
        rows = []
        for method in (("spec",) + (("dp", "dpgrid") if dp else ())):
            wrong = clip = 0
            errs = []
            f1s = []
            trans = 0
            for style in STYLES:
                for bpm in bpms:
                    odf, mags, cand = prepared[(style, bpm)]
                    odn = odf / max(1e-30, float(np.max(odf)))
                    sbpm, mag, scands = spectral_pick(mags, lo, hi, tie_db)
                    if method == "spec":
                        est = sbpm
                        tb = trans
                        beats = np.array([], dtype=np.int64)
                    elif method == "dp":
                        # the DP runs at the periods the SPECTRUM proposed:
                        # the top few local peaks of the band.
                        klo, khi = band_bins(lo, hi)
                        peaks = local_peaks_strict(mags, klo, khi)
                        peaks.sort(key=lambda t: -t[1])
                        props = [60.0 * RATE / b for b, _ in peaks[:6] if b > 0]
                        if not props:
                            props = [60.0 * RATE / (0.5 * (lo + hi))]
                        p, beats, tb = dp_pick(cand, odn, props)
                        est = (60.0 * RATE / p) if p else None
                    else:
                        # Ellis's full tempo search: the DP at EVERY period of
                        # a 1-frame grid over the band.
                        pmax = 60.0 * RATE / lo
                        pmin = 60.0 * RATE / hi
                        grid = list(np.arange(pmin, pmax + 1.0, 1.0))
                        p, beats, tb = dp_pick(cand, odn, grid)
                        est = (60.0 * RATE / p) if p else None
                    if method != "spec":
                        # the tempo that is actually published: the period of
                        # the beat SEQUENCE, not the DP's period parameter.
                        bp = beat_period(beats)
                        if bp > 0.0:
                            est = 60.0 * RATE / bp
                    tb_ = true_beats(bpm, 16.0)
                    if est is None:
                        wrong += 1
                        errs.append(999.0)
                        f1s.append(0.0)
                    else:
                        e = abs(est - bpm)
                        errs.append(e)
                        if e >= 2.0:
                            wrong += 1
                            if (est > hi + 2.0) or (est < lo - 2.0):
                                clip += 1
                        f1s.append(f1_score(beats, tb_, F1_TOL * RATE))
                    trans += tb
            n = len(STYLES) * len(bpms)
            results[(name, method)] = (wrong, clip, n)
            print("%-9s %-9s %7d %7d %7.2f %9.3f %9.0f" %
                  (name, method, wrong, clip, sum(errs) / n,
                   sum(f1s) / n, trans / n))
    return results


def cost_report():
    """The numbers that become CPU."""
    print("%-10s %6s %8s %10s %10s %10s" %
          ("style", "bpm", "cands", "cands/s", "trans(1dp)", "trans(4dp)"))
    tot_c = tot_t1 = tot_t4 = 0
    for style in STYLES:
        for bpm in (90, 120, 150, 174):
            odf, mags, cand = build(style, bpm)
            odn = odf / max(1e-30, float(np.max(odf)))
            p = 60.0 * RATE / bpm
            _, t1, _ = dp_beats(cand, odn, p)
            klo, khi = band_bins(90.0, 175.0)
            peaks = local_peaks_strict(mags, klo, khi)
            peaks.sort(key=lambda t: -t[1])
            props = [60.0 * RATE / b for b, _ in peaks[:4] if b > 0]
            _, _, t4 = dp_pick(cand, odn, props)
            tot_c += len(cand)
            tot_t1 += t1
            tot_t4 += t4
            print("%-10s %6d %8d %10.2f %10d %10d" %
                  (style, bpm, len(cand), len(cand) / 16.0, t1, t4))
    n = len(STYLES) * 4
    print("mean candidates %.0f   mean transitions: 1 DP %d, 4 DP %d"
          % (tot_c / n, tot_t1 / n, tot_t4 / n))


def filter_shootout():
    """Which band-pass does the firmware use?

    Every design is measured on the same 48 tracks with the same selection
    rule, so the only thing that changes is the filter.
    """
    bpms = (90, 100, 120, 128, 140, 150, 160, 174)
    print("%-14s %6s  %s" % ("design", "wrong", "per-style"))
    designs = filter_designs()
    for name in designs:
        if name == "ideal":
            continue
        counts = {}
        wrong = 0
        worst = 0.0
        for style in STYLES:
            w = 0
            for bpm in bpms:
                x = get_track(style, bpm, 0.0, 0.0)
                odf = odf_fast(x, designs[name])
                mags = running_dft_grid(odf)
                bpm_est, mag, _ = spectral_pick(mags, 90.0, 175.0)
                if bpm_est is None or abs(bpm_est - bpm) >= 2.0:
                    w += 1
                    wrong += 1
                    worst = max(worst, abs((bpm_est or 0) - bpm))
            counts[style] = w
        print("%-14s %6d  %s   worst %.2f" %
              (name, wrong, " ".join("%s:%d" % (k[:5], v) for k, v in counts.items()),
               worst))


def candidate_report():
    """How many onset candidates a track really has, and how many DP
    transitions one DP over them costs."""
    bpms = (90, 100, 120, 128, 140, 150, 160, 174)
    tot_c = tot_t = 0
    n = 0
    worst_c = 0
    worst_t = 0
    for style in STYLES:
        for bpm in bpms:
            odf, mags, cand = build(style, bpm)
            odn = odf / max(1e-30, float(np.max(odf)))
            klo, khi = band_bins(90.0, 175.0)
            peaks = local_peaks_strict(mags, klo, khi)
            peaks.sort(key=lambda t: -t[1])
            props = [60.0 * RATE / b for b, _ in peaks[:6] if b > 0]
            _, _, t = dp_pick(cand, odn, props)
            tot_c += len(cand)
            tot_t += t
            worst_c = max(worst_c, len(cand))
            worst_t = max(worst_t, t)
            n += 1
    print("tracks %d   mean candidates %.1f   worst %d" % (n, tot_c / n, worst_c))
    print("mean transitions for 6 DPs %.0f   worst %d" % (tot_t / n, worst_t))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bands", action="store_true")
    ap.add_argument("--dp", action="store_true")
    ap.add_argument("--cost", action="store_true")
    ap.add_argument("--filters", action="store_true")
    ap.add_argument("--cands", action="store_true")
    ap.add_argument("--tie", type=float, default=3.0)
    ap.add_argument("--human", type=float, default=0.0)
    ap.add_argument("--noise", type=float, default=0.0)
    ap.add_argument("--skip", type=float, default=0.0)
    args = ap.parse_args()
    if args.cost:
        cost_report()
    if args.cands:
        candidate_report()
    if args.filters:
        filter_shootout()
    if args.bands:
        sweep(dp=args.dp, human=args.human, noise=args.noise, skip=args.skip, tie_db=args.tie)
    if not (args.bands or args.cost or args.filters or args.cands):
        ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())

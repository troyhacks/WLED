#!/usr/bin/env python3
"""The phase-locked comb score: the rate estimator.

THE MEASUREMENT THAT PROVED IT
------------------------------
The autocorrelation of the onset envelope turned out to be nearly flat
noise on real music - on "Dennis Quin - Bootycall" the true period sat
7.4 dB BELOW the largest value in the whole lag band, and that largest
value was at the band edge.  An autocorrelation measures periodicity; it
does not require the onsets to LAND on a grid, and on a dense envelope
they do not.

The comb score does require it.  For a candidate period P, every onset's
phase offset within the period is u_i = t_i mod P.  If the onsets are a
clean beat train, their offsets CLUSTER - and the size of that cluster
is the score.  If the period is wrong, the offsets smear around the
circle and the score collapses.

Two things make this the right estimator here:

  * It resolves the rate to about 0.75 BPM over a 12 s window.  A rate
    error of e drifts the grid by e x 12 s, so to stay inside a 70 ms
    tolerance the error must be under 0.58%.  That is far finer than the
    autocorrelation's integer lag grid (1.7 BPM steps at 128 BPM) and
    far finer than its parabolic refinement, which biases low.

  * It rejects the wrong metrical level for free.  A 3:2 grid does not
    align with a beat train at all - the alignment drifts at 0.63 cycles
    a second and washes out within a second.  Only an EXACT 2:1 can
    survive, and the 90-175 BPM band excludes that for anything above
    90 BPM.

The score is the peak of the smoothed phase histogram, normalised by the
total onset strength, so it is a FRACTION: 1.0 means every onset's
strength sits under the best phase.  That makes it comparable across
rates, which the raw autocorrelation sum is not.

THE PRIOR
---------
A comb at half the true period also covers every onset, so the score
favours the fastest rate in the band.  The band removes the octave for
anything above 90 BPM; the log-normal prior removes the rest, and it is
where Troy's 90-175 BPM range and 128-135 working range enter.

    python tools/ar_sim/_comb.py --limit 60
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _tempo_acf as T                             # noqa: E402
import _beat as B                                  # noqa: E402


def phase_histogram(frames, strengths, period, n_bins=256, tol_frames=6.6):
    """The smoothed circular histogram of the onsets' phase offsets.

    Returns (score, phase_frames, total_strength).  The score is the
    fraction of the onset strength that falls within `tol_frames` of the
    best phase.
    """
    total = float(np.sum(strengths))
    if total <= 0 or period <= 0 or not len(frames):
        return 0.0, 0.0, total
    u = np.mod(frames, period)
    bins = np.floor(u / period * n_bins).astype(int)
    bins = np.clip(bins, 0, n_bins - 1)
    h = np.zeros(n_bins)
    np.add.at(h, bins, strengths)

    # a box kernel of half-width tol, wrapped.  It is a SUM, not an
    # average: the window's width in PHASE is the same for every rate
    # (2*tol frames), so the sum in the window is the onset strength that
    # sits under that phase, and dividing by the total makes it the
    # FRACTION of the onset strength that is covered.
    w = max(1, int(round(tol_frames / period * n_bins)))
    k = np.ones(2 * w + 1)
    hs = np.zeros(n_bins)
    for i in range(n_bins):
        idx = (np.arange(i - w, i + w + 1)) % n_bins
        hs[i] = float(np.dot(h[idx], k))
    j = int(np.argmax(hs))
    return float(hs[j] / total), float(j / n_bins * period), total


def comb_rate(frames, strengths, bpm_lo=90.0, bpm_hi=175.0, step=0.25,
              tol_s=0.070, prior_bpm=125.0, prior_sigma=0.35,
              prior_weight=0.25, n_bins=256, refine=0.0):
    """The rate, by the phase-locked comb score.

    Returns (bpm, score, phase_frames, detail).

    `refine` is the half-width, in BPM, of a parabolic fit to the score's
    peak.  The score's maximum is a BROAD, SHALLOW bump: the grid drifts
    by rate_error x window, so the score only starts to fall when the
    drift reaches the 70 ms tolerance, which at 12 s is 0.58%.  The
    argmax of a bump that broad, sampled every `step` BPM, is quantised
    to the step - and a quarter of a BPM is 0.19% at 128 BPM, which is
    23 ms of drift over 12 s.  A third of the tolerance, thrown away for
    free.

    Fitting a parabola to the three samples around the maximum recovers
    the peak's position to a small fraction of the step, and the drift
    with it.
    """
    tol_frames = tol_s * T.RATE
    best = None
    table = []
    n = int(round((bpm_hi - bpm_lo) / step))
    for i in range(n + 1):
        bpm = bpm_lo + i * step
        period = 60.0 * T.RATE / bpm
        score, phase, _tot = phase_histogram(frames, strengths, period,
                                              n_bins=n_bins,
                                              tol_frames=tol_frames)
        pen = prior_weight * (np.log(bpm / prior_bpm) / prior_sigma) ** 2
        table.append((score - pen, score, pen, bpm, phase))
        if best is None or (score - pen) > best[0]:
            best = (score - pen, score, pen, bpm, phase)
    best = (best[0], best[1], best[2], best[3], best[4])

    if refine > 0.0:
        bpm = best[3]
        sm, _pm, _tm = phase_histogram(frames, strengths,
                                        60.0 * T.RATE / (bpm - refine),
                                        n_bins=n_bins, tol_frames=tol_frames)
        sp, _pp, _tp = phase_histogram(frames, strengths,
                                        60.0 * T.RATE / (bpm + refine),
                                        n_bins=n_bins, tol_frames=tol_frames)
        denom = sm - 2.0 * best[1] + sp
        if denom < -1e-9:
            shift = 0.5 * refine * (sm - sp) / denom
            shift = max(-refine, min(refine, shift))
            nb = bpm + shift
            nsc, nph, _t = phase_histogram(frames, strengths,
                                            60.0 * T.RATE / nb,
                                            n_bins=n_bins,
                                            tol_frames=tol_frames)
            best = (nsc, nsc, best[2], nb, nph)

    return best[3], best[1], best[4], {"table": table}


def comb_beats(bpm, phase_frames, t_lo, t_hi):
    """The regular grid the comb score chose."""
    period = 60.0 * T.RATE / bpm
    k0 = int(np.floor((t_lo - phase_frames) / period))
    k1 = int(np.ceil((t_hi - phase_frames) / period))
    g = phase_frames + np.arange(k0, k1 + 1) * period
    return g


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=60)
    ap.add_argument("--design", default="hp40_lp160x2")
    ap.add_argument("--whiten", type=float, default=0.0)
    ap.add_argument("--step", type=float, default=0.25)
    ap.add_argument("--prior", type=float, default=125.0)
    ap.add_argument("--weight", type=float, default=0.25)
    ap.add_argument("--show", type=int, default=12)
    args = ap.parse_args()

    keys = _cache.keys()[:args.limit]
    win = 1125
    ours = []
    oracles = []
    rows = []
    for k in keys:
        x = _cache.load(k)
        if x is None:
            continue
        odf = T.odf_cached(k, x, args.design, args.whiten)
        mx = float(np.max(odf))
        odfn = odf / mx if mx > 0 else odf
        peaks, strengths = B.peak_pick(odfn)
        first = max(0, len(odfn) - win)
        keep = peaks >= first
        peaks = peaks[keep]
        strengths = strengths[keep]
        if len(peaks) < 8:
            continue
        bpm, score, phase, det = comb_rate(peaks, strengths, step=args.step,
                                           prior_bpm=args.prior,
                                           prior_weight=args.weight)
        t_lo = first / T.RATE
        t_hi = len(odfn) / T.RATE

        from _diag_phase import kick_onsets
        from _diag_grid import hit_frac
        ons = kick_onsets(x, _cache.CACHE_RATE, _cache.CACHE_HOP)
        wo = ons[(ons >= t_lo - 0.02) & (ons <= t_hi)]
        if len(wo) < 4:
            continue
        grid = comb_beats(bpm, phase, t_lo * T.RATE, t_hi * T.RATE) / T.RATE
        f = hit_frac(grid, wo)
        ours.append(f)
        rows.append((k, bpm, score, f))

    a = np.array(ours)
    print("%d windows   comb step %.2f BPM   prior %.0f weight %.2f"
          % (len(rows), args.step, args.prior, args.weight))
    print("  hit rate median %5.3f   >=0.5: %5.1f%%   >=0.8: %5.1f%%"
          % (float(np.median(a)), 100.0 * float((a >= 0.5).mean()),
             100.0 * float((a >= 0.8).mean())))
    print()
    rows.sort(key=lambda r: r[3])
    for k, bpm, score, f in rows[:args.show]:
        print("  %-44s bpm %7.2f  comb score %5.3f  hit %5.3f"
              % (k[:44], bpm, score, f))


if __name__ == "__main__":
    sys.exit(main())

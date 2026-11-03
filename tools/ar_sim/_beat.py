#!/usr/bin/env python3
"""Ellis' dynamic-programming beat tracker, and the tempo estimate it implies.

WHY THIS EXISTS
---------------
The autocorrelation says "there is strong periodicity at 160.7 BPM" on a
track whose beat is at 122 BPM.  Peak height cannot fix that: in house
music the offbeat bass is often as loud as the kick, so the ODF at the
midpoint between beats is >70% of the ODF at the beat in 80% of the
windows.  A peak picker is choosing between two levels of the SAME
rhythm, and the taller one is not always the one a human counts.

What DOES resolve it is a beat tracker.  Ellis' DP (2007) scores a beat
sequence by two things at once:

  * the onset strength at each chosen beat, and
  * how CONSISTENT the spacing between consecutive beats is.

A grid at 160.7 BPM on a 122 BPM track only scores well if the music
really has an onset every 0.373 s.  If it does not, the DP has to reach
back further to fill the grid, the inter-beat intervals become
irregular, and the transition penalty eats the score.  The beat level
falls out of the data instead of being assumed.

That is the whole point: the tempo is not a peak, it is the period of
the beat sequence the DP finds.

THE PRIOR
---------
Comparing candidates by the MEAN onset strength at the beats (not the
total) removes the "more beats = more score" bias, but leaves the
"fewer beats = higher mean" bias.  Ellis' answer is a log-normal tempo
prior: candidates far from the prior's median are penalised.  This is
where the 90-175 BPM band and Troy's 128-135 range enter - as the prior
that says which level of the rhythm to prefer when the music itself is
ambiguous.  It is a PRODUCT decision, stated here, not a hidden tuning.

    python tools/ar_sim/_beat.py --keys "018."
"""

import argparse
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _tempo_acf as T                             # noqa: E402

# --- the beat tracker ---------------------------------------------------

# Ellis' F1 tolerance.  A beat is "the same beat" if it is within 70 ms.
TOL = 0.070

# The prior.  Median period = 60/125 s, i.e. 125 BPM, with a log-normal
# spread of PRIOR_SIGMA (in natural log units) either side.  0.35 in ln
# is about +/-27%, which covers 98-160 BPM at one sigma.
PRIOR_BPM = 125.0
PRIOR_SIGMA = 0.35
PRIOR_WEIGHT = 1.0        # how hard the prior pulls, in ODF-normalised units


def peak_pick(odf, delta=0.35, pre=0.03, post=0.03, wait=0.05, floor=0.06):
    """Onset peaks from the ODF, the way a firmware peak picker would.

    A frame is a peak when it is the maximum of a +/- `pre`/`post` window
    (the local-maximum test), stands `delta` above the LOCAL MEAN of the
    ODF (the adaptive threshold - a fixed floor would drown in a loud
    passage and vanish in a quiet one), and is not within `wait` of a
    stronger peak.

    The strengths that come back are scaled so the MEDIAN peak is 1.0.
    That puts the onset strength in the same units the DP's transition
    penalty is quoted in, which is the only way to set that weight
    sensibly.
    """
    n = len(odf)
    if n == 0:
        return np.zeros(0, dtype=int), np.zeros(0)
    mx = float(np.max(odf))
    if mx <= 0:
        return np.zeros(0, dtype=int), np.zeros(0)
    o = odf / mx
    pf = max(1, int(round(pre * T.RATE)))
    po = max(1, int(round(post * T.RATE)))
    wf = max(1, int(round(wait * T.RATE)))
    lf = max(2, int(round(0.35 * T.RATE)))

    # the local mean, by a running sum - the firmware's equivalent
    cs = np.concatenate(([0.0], np.cumsum(o)))
    cands = []
    for i in range(n):
        lo = max(0, i - pf)
        hi = min(n, i + po + 1)
        if o[i] < floor:
            continue
        if o[i] < o[lo:hi].max():
            continue
        a = max(0, i - lf)
        b = min(n, i + lf + 1)
        lm = (cs[b] - cs[a]) / (b - a)
        if o[i] < lm * (1.0 + delta):
            continue
        cands.append(i)

    # enforce the minimum separation, keeping the stronger of any pair
    picked = []
    for i in cands:
        if picked and (i - picked[-1]) < wf:
            if o[i] > o[picked[-1]]:
                picked[-1] = i
            continue
        picked.append(i)
    picked = np.asarray(picked, dtype=int)
    if not len(picked):
        return picked, np.zeros(0)
    s = o[picked]
    med = float(np.median(s))
    if med > 0:
        s = s / med
    return picked, s


def dp_beat_track(times, strengths, period, tol=0.20, tightness=60.0):
    """Ellis' DP.  `times` and `strengths` are the onset peaks.

    Returns (path_indices, mean_strength_at_the_beats).

    The transition term is tightness * -ln(dt/period), so an interval
    exactly `period` long costs nothing and one twice or half as long
    costs -tightness*ln(2) = -42 at tightness 60.  That is what makes
    the tracker prefer a consistent grid over a dense one.
    """
    n = len(times)
    if n < 2 or period <= 0:
        return np.zeros(0, dtype=int), 0.0
    lo = period * (1.0 - tol)
    hi = period * (1.0 + tol)
    cum = np.zeros(n)
    back = np.full(n, -1, dtype=int)
    for i in range(n):
        best = -1e30
        bj = -1
        ti = times[i]
        # times[] is sorted, so the predecessors whose gap is in [lo, hi]
        # are a contiguous slice.  searchsorted finds it in O(log n)
        # instead of scanning every earlier onset.
        j_lo = int(np.searchsorted(times, ti - hi, side="left"))
        j_hi = int(np.searchsorted(times, ti - lo, side="right"))
        for j in range(j_lo, min(j_hi, i)):
            dt = ti - times[j]
            s = cum[j] + tightness * (-math.log(dt / period))
            if s > best:
                best = s
                bj = j
        if bj < 0:
            cum[i] = strengths[i]
            back[i] = -1
        else:
            cum[i] = strengths[i] + best
            back[i] = bj
    end = int(np.argmax(cum))
    # The best path must REACH THE END of the onset sequence.  Taking the
    # global argmax of the cumulative score lets a lucky early chain win,
    # and the path then covers only a few beats of the window.  Ellis'
    # DP ends at the last onset; a small allowance lets the path stop
    # early if the last few onsets are weak.
    tail = max(1, n // 10)
    end = int(np.argmax(cum[n - tail:])) + (n - tail)
    path = []
    k = end
    while k >= 0:
        path.append(k)
        k = int(back[k])
    path.reverse()
    if not path:
        return np.zeros(0, dtype=int), 0.0
    p = np.asarray(path, dtype=int)
    return p, float(np.mean(strengths[p]))


def candidates_from_acf(lags, r, top=10, min_db=-14.0):
    """The ACF's local maxima, as candidate periods (in frames).

    The ACF is a candidate generator, not the answer.  Every local
    maximum above the floor is offered to the DP, which then decides.
    """
    if len(r) < 3:
        return []
    ref = float(np.max(r))
    if ref <= 0:
        return []
    out = []
    for i in range(1, len(r) - 1):
        if r[i] <= r[i - 1] or r[i] < r[i + 1]:
            continue
        if 20.0 * math.log10(max(r[i], 1e-9) / ref) < min_db:
            continue
        # parabolic refinement for a sub-lag estimate
        y0, y1, y2 = r[i - 1], r[i], r[i + 1]
        den = (y0 - 2.0 * y1 + y2)
        d = 0.0 if abs(den) < 1e-12 else 0.5 * (y0 - y2) / den
        lag = lags[i] + d
        out.append((float(r[i]), float(lag)))
    out.sort(key=lambda t: -t[0])
    return out[:top]


def estimate_dp(odf, win=None, top=10, prior_bpm=PRIOR_BPM, prior_weight=PRIOR_WEIGHT,
                tol=0.20, tightness=1.0, min_beats=8, odf_norm=None):
    """The tempo estimate: ACF candidates, DP-scored, prior-regularised.

    Returns (bpm, confidence, detail).
    """
    detail = {}
    win = win if win else T.WIN
    if odf_norm is None:
        mx = float(np.max(odf)) if len(odf) else 0.0
        odf_norm = odf / mx if mx > 0 else odf
    peaks, strengths = peak_pick(odf_norm)
    detail["npeaks"] = len(peaks)
    if len(peaks) < min_beats:
        return None, 0.0, detail

    # the ACF and the DP must see the SAME window, or the candidates come
    # from one stretch of music and the beats from another
    first = max(0, len(odf_norm) - win)
    keep = peaks >= first
    peaks = peaks[keep]
    strengths = strengths[keep]
    if len(peaks) < min_beats:
        return None, 0.0, detail

    lags, r = T.acf_of(odf_norm, win)
    cands = candidates_from_acf(lags, r, top=top)
    detail["ncands"] = len(cands)
    if not cands:
        return None, 0.0, detail

    best = None
    scored = []
    for mag, lag in cands:
        period = lag
        if period <= 0:
            continue
        path, mean_s = dp_beat_track(peaks, strengths, period, tol=tol,
                                      tightness=tightness)
        if len(path) < min_beats:
            continue
        bpm = 60.0 * T.RATE / period
        # the prior: a log-normal on the BPM, centred on prior_bpm
        pen = prior_weight * (math.log(bpm / prior_bpm) / PRIOR_SIGMA) ** 2
        score = mean_s - pen
        scored.append((score, bpm, mean_s, pen, len(path)))
        if best is None or score > best[0]:
            best = (score, bpm, mean_s, pen, len(path))
    if best is None:
        detail["scored"] = []
        return None, 0.0, detail

    scored.sort(key=lambda t: -t[0])
    detail["scored"] = scored
    # the confidence is the winner's margin over the runner-up, in the
    # same units as the onset strength (a median onset = 1.0)
    if len(scored) > 1:
        margin = scored[0][0] - scored[1][0]
    else:
        margin = scored[0][2]
    detail["nbeats"] = scored[0][4]
    return best[1], margin, detail


def f1_against(est_beats, oracle_beats, tol=TOL):
    """Ellis' F1 between two beat lists (times in seconds)."""
    if not len(est_beats) or not len(oracle_beats):
        return 0.0, 0
    e = np.asarray(est_beats, dtype=float)
    o = np.asarray(oracle_beats, dtype=float)
    tp = 0
    used = np.zeros(len(o), dtype=bool)
    for t in e:
        d = np.abs(o - t)
        j = int(np.argmin(d))
        if d[j] <= tol and not used[j]:
            used[j] = True
            tp += 1
    prec = tp / len(e)
    rec = tp / len(o)
    f1 = 0.0 if (prec + rec) == 0 else 2 * prec * rec / (prec + rec)
    return f1, tp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keys", default=None)
    ap.add_argument("--design", default="hp40_lp160x2")
    ap.add_argument("--whiten", type=float, default=0.0)
    ap.add_argument("--window", type=float, default=T.WINDOW_S)
    ap.add_argument("--top", type=int, default=10)
    ap.add_argument("--prior", type=float, default=PRIOR_BPM)
    ap.add_argument("--weight", type=float, default=PRIOR_WEIGHT)
    ap.add_argument("--show", action="store_true")
    args = ap.parse_args()

    keys = _cache.keys()
    if args.keys:
        keys = [k for k in keys if args.keys in k]
    win = int(args.window * T.RATE)

    import _oracle_librosa                            # noqa: E402
    n = 0
    f1s = []
    for k in keys:
        o = _oracle_librosa.load(k)
        x = _cache.load(k)
        if o is None or x is None or not o.get("beats"):
            continue
        odf = T.odf_cached(k, x, args.design, args.whiten)
        bpm, conf, det = estimate_dp(odf, win=win, top=args.top,
                                     prior_bpm=args.prior,
                                     prior_weight=args.weight)
        if bpm is None:
            continue
        n += 1
        # the estimator's own beat times, for the F1 - over the SAME
        # window the ACF and the DP saw, so the two are comparable
        mx = float(np.max(odf))
        odfn = odf / mx if mx > 0 else odf
        peaks, strengths = peak_pick(odfn)
        first = max(0, len(odfn) - win)
        keep = peaks >= first
        peaks = peaks[keep]
        strengths = strengths[keep]
        path, _m = dp_beat_track(peaks, strengths, 60.0 * T.RATE / bpm)
        est_beats = peaks[path] / T.RATE
        span = (len(odfn) - first) / T.RATE
        t0 = (len(odfn) - first) / T.RATE
        oracle = [b for b in o["beats"] if b >= t0 - 0.5]
        f1, tp = f1_against(est_beats, oracle)
        f1s.append(f1)
        if args.show:
            print("  %-44s bpm %7.2f  conf %6.3f  beats %3d  F1 %5.3f  lib %7.2f"
                  % (k[:44], bpm, conf, len(path), f1, o["bpm"]))

    if f1s:
        f = np.array(f1s)
        print("%d windows   F1 mean %5.3f  median %5.3f   F1>0.5: %5.1f%%   "
              "F1>0.8: %5.1f%%"
              % (len(f), float(f.mean()), float(np.median(f)),
                 100.0 * float((f > 0.5).mean()), 100.0 * float((f > 0.8).mean())))


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""The QUALITY, the DRIFT and the fake half-BPM steps.

Three questions, one instrument:

1. Does the quality fall when the music stops explaining the grid?  The
   synthetic breakdown is the clean case: 128 for twenty seconds, five
   seconds of nothing, 128 again.  The rate must not move, and the GRID
   must not move either - the grid's beats have to stay where they were.

2. How far does a locked grid go out of sync?  The lock freezes the clock
   and the estimate keeps running, so the drift is the distance between
   the frozen grid and the music.

3. Where do the fake half-BPM steps come from?  The published rate's steps
   are counted, and each step is looked at: is it a step the measurement
   kept making (a real change) or one the lattice's own quantisation
   invented?

    python tools/ar_sim/_bg_qual.py --case all
"""

import argparse
import json
import math
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_bg as NEW                                # noqa: E402
import _fw_port as P                               # noqa: E402

ORACLE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "_oracle")


def oracle(k):
    """librosa's tempo and beats for this cached window."""
    path = os.path.join(ORACLE_DIR, k + ".json")
    try:
        with open(path, "r") as f:
            return json.load(f)
    except Exception:
        return None


def oracle_bpm(k):
    o = oracle(k)
    return float(o.get("bpm", 0.0)) if o else 0.0


def oracle_beats(k):
    o = oracle(k)
    return o.get("beats", []) if o else []


def grid_deviation(t, lo, hi):
    """How far the grid's beats are from a PERFECT grid, in ms.

    The perfect grid is fitted to the tracker's own beats OUTSIDE the
    window, so what is measured is the grid's movement DURING the window,
    not its absolute phase.
    """
    beats = np.array([b for b in t.beats if lo <= b / P.RATE <= hi],
                     dtype=float)
    if len(beats) < 4:
        return 0.0, 0.0, 0
    # the reference grid: the median period and the phase of the first beat
    per = float(np.median(np.diff(beats)))
    if per <= 0:
        return 0.0, 0.0, len(beats)
    k = np.round((beats - beats[0]) / per)
    dev = (beats - beats[0]) - k * per
    return float(np.max(np.abs(dev)) / P.RATE * 1000.0), \
        float(np.std(dev) / P.RATE * 1000.0), len(beats)


def case_breakdown():
    """The breakdown: the rate must not move, the GRID must not move."""
    print("== the breakdown: 128 for 20 s, five seconds of nothing, 128 again")
    odf = NEW.beat_odf([(128.0, 0.0, 20.0, 1.0),
                        (128.0, 25.0, 20.0, 1.0)], dur=45.0)
    t = NEW.run(odf)
    # the quality, sampled over the whole run
    q = np.array(t.qual_log) if getattr(t, "qual_log", None) else None
    print("   the rate: before %7.2f   during %7.2f   after %7.2f"
          % (NEW.med(np.array(t.bpm_log), 15, 19),
             NEW.med(np.array(t.bpm_log), 21, 24),
             NEW.med(np.array(t.bpm_log), 30)))
    mx, sd, n = grid_deviation(t, 21.0, 24.0)
    mx0, sd0, n0 = grid_deviation(t, 10.0, 19.0)
    print("   the grid during the breakdown: max %6.1f ms   sd %6.1f ms"
          "   (%d beats)" % (mx, sd, n))
    print("   the grid before it:             max %6.1f ms   sd %6.1f ms"
          "   (%d beats)" % (mx0, sd0, n0))
    if q is not None:
        print("   the quality: before %5.2f   during %5.2f   after %5.2f"
              % (NEW.med(q, 15, 19), NEW.med(q, 21, 24), NEW.med(q, 30)))
    return t


def case_lock():
    """The lock: the clock freezes, the estimate keeps running, and the
    drift says how far the frozen grid is from the music."""
    print("== the lock: the clock frozen while the music goes on")
    odf = NEW.beat_odf([(128.0, 0.0, 20.0, 1.0),
                        (132.0, 20.0, 20.0, 1.0)], dur=40.0)
    t = NEW.run(odf)
    # now the same music with the lock on from the start
    t2 = NEW.BgTracker()
    t2.hold = True
    t2.run(odf)
    log = np.array(t2.bpm_log)
    print("   unlocked: at 30 s %7.2f   (the music is 132)" % log[-1])
    print("   locked:   at 30 s %7.2f   drift %6.1f ms   rate drift %5.2f BPM"
          % (log[-1], t2.drift_ms, t2.drift_bpm))
    return t2


def corpus(n=60, trace=False):
    """The real music: the quality, the drift and the rate's steps."""
    print("== the corpus: %d windows" % n)
    t0 = time.time()
    qs = []
    ds = []
    steps = {}
    moves = []
    orc_errs = []
    fracs = []
    hit = []
    for k in _cache.keys()[:n]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = NEW.odf_of(x)
        if len(odf) < 1200:
            continue

        class F(NEW.BgTracker):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                self.q = []
                self.d = []
                self.ans = []

            def narrow_result(self, rate, phase, score, local):
                super().narrow_result(rate, phase, score, local)
                self.q.append(self.qual)
                self.d.append(self.drift_ms)
                self.ans.append(rate)

        t = F()
        t.run(odf)
        if not t.locked:
            continue
        qs.append(np.median(t.q))
        ds.append(float(np.median(np.abs(t.d))))
        # the oracle: librosa's own tempo for this window
        orc = oracle_bpm(k)
        if orc > 0:
            med_rate = float(np.median(t.bpm_log[int(len(t.bpm_log) * 0.75):]))
            orc_errs.append(abs(med_rate - orc))
            fracs.append(med_rate - math.floor(med_rate))
            # the oracle's own grid: how well do OUR beats line up with
            # librosa's?
            beats = np.array([b / P.RATE for b in t.beats], dtype=float)
            if len(beats):
                ob = np.array(oracle_beats(k), dtype=float)
                if len(ob):
                    d = np.abs(ob[:, None] - beats[None, :]).min(axis=1)
                    hit.append(float((d <= 0.070).mean()))
        # the PUBLISHED rate's steps, one second apart
        log = np.array(t.bpm_log)
        fps = int(round(P.RATE))
        per = [float(np.median(log[i:i + fps]))
               for i in range(0, len(log) - fps, fps)]
        st = np.abs(np.diff(per)) if len(per) > 1 else np.array([0.0])
        moves.append(int((st > 0.25).sum()))
        for v in st[st > 0.05]:
            key = round(float(v) * 4.0) / 4.0
            steps[key] = steps.get(key, 0) + 1
        # the MEASUREMENT's steps: the candidate rate's own jumps
        ans = np.array([a for a in t.ans if a > 0])
        if len(ans) > 4:
            d = np.abs(np.diff(ans))
            d = d[d > 0.05]
            for v in d:
                key = round(float(v) * 4.0) / 4.0
                steps[("m", key)] = steps.get(("m", key), 0) + 1
    q = np.array(qs)
    d = np.array(ds)
    print("   %d windows   %.0f s" % (len(q), time.time() - t0))
    print("   the quality: median %5.2f   10th %5.2f   90th %5.2f"
          % (np.median(q), np.percentile(q, 10), np.percentile(q, 90)))
    print("   the drift:   median %6.1f ms   90th %6.1f ms"
          % (np.median(d), np.percentile(d, 90)))
    oe = np.array(orc_errs)
    if len(oe):
        print("   the rate against librosa's: median %5.3f   mean %5.3f   "
              "90th %5.3f BPM" % (np.median(oe), oe.mean(),
                                  np.percentile(oe, 90)))
        fr = np.array(fracs)
        print("   the rate's fractional part: median %5.3f   "
              "on a quarter %d of %d"
              % (np.median(fr), int((np.abs(fr * 4 - np.round(fr * 4))
                                     < 0.02).sum()), len(fr)))
    if hit:
        hh = np.array(hit)
        print("   our beats against librosa's: median %5.1f %%   10th %5.1f %%"
              % (np.median(hh) * 100.0, np.percentile(hh, 10) * 100.0))
    print("   the published rate's steps (BPM):")
    for k in sorted([k for k in steps if not isinstance(k, tuple)],
                    reverse=True)[:8]:
        print("      %5.2f   %d" % (k, steps[k]))
    print("   the MEASUREMENT's steps (BPM):")
    for k in sorted([k for k in steps if isinstance(k, tuple)],
                    key=lambda z: -z[1])[:8]:
        print("      %5.2f   %d" % (k[1], steps[k]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--case", default="all")
    ap.add_argument("--n", type=int, default=60)
    args = ap.parse_args()
    which = args.case
    if which in ("all", "breakdown"):
        case_breakdown()
    if which in ("all", "lock"):
        case_lock()
    if which in ("all", "corpus"):
        corpus(args.n)
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""THE RATE: the plateau's centre, or the least-squares fit?

Troy: *"Also locking on 0.5 increments of fake BPM changes sometimes."*

The comb's score SATURATES - a whole band of nearby rates captures every
onset - so the rate the tracker published was the CENTRE of that band,
which can only land on the lattice's own step.  The alternative is a FIT:
the onsets that sit on the candidate's grid are the beats, and the least
squares line through them gives the period the music really has.  The fit
is continuous, so it cannot step by half a BPM.

The measures, on the real music in the cache:

  err     the published rate against librosa's tempo for the same window
  wobble  how much the published rate moves over the last ten seconds
  beat    the fraction of LIBROSA's beats that our grid explains - the
          sharpest test there is, because it is another implementation's
          beat grid, not another number
  quant   how often the published rate lands on a quarter of a BPM

    python tools/ar_sim/_bg_refine.py --n 60
"""

import argparse
import json
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
    path = os.path.join(ORACLE_DIR, k + ".json")
    try:
        with open(path, "r") as f:
            return json.load(f)
    except Exception:
        return None


def measure(odf, orc, **kw):
    t = NEW.BgTracker()
    for k, v in kw.items():
        setattr(t, k, v)
    t.run(odf)
    if not t.locked or t.bpm <= 0:
        return None
    log = np.array(t.bpm_log)
    n = len(log)
    fps = int(round(P.RATE))
    tail = log[int(n - 10 * fps):]
    if len(tail) == 0:
        return None
    per = [float(np.median(log[i:i + fps]))
           for i in range(0, len(tail) - fps, fps)]
    steps = np.abs(np.diff(per)) if len(per) > 1 else np.array([0.0])
    med = float(np.median(tail))
    out = dict(wobble=float(tail.max() - tail.min()),
               moves=int((steps > 0.25).sum()),
               med=med)
    if orc:
        out["err"] = abs(med - float(orc.get("bpm", 0.0)))
        ob = np.array(orc.get("beats", []), dtype=float)
        beats = np.array([b / P.RATE for b in t.beats], dtype=float)
        if len(ob) and len(beats):
            d = np.abs(ob[:, None] - beats[None, :]).min(axis=1)
            out["beat"] = float((d <= 0.070).mean())
        else:
            out["beat"] = 0.0
    fr = med - np.floor(med)
    out["quant"] = 1 if abs(fr * 4 - np.round(fr * 4)) < 0.02 else 0
    # the grid's own quality: how much of the music's onsets ONE offset
    # explains.  This is the sharpest measure there is - it is about the
    # grid, not about a number.
    onsets = np.array([f for f, _ in t.peak_frame], dtype=float) / P.RATE
    beats = np.array([b for b in t.beats], dtype=float) / P.RATE
    if len(onsets) and len(beats):
        out["hit"] = P.hit_with_offset(beats, onsets)[0]
    else:
        out["hit"] = 0.0
    return out


def report(name, rows):
    if not rows:
        print("   %s: nothing" % name)
        return
    w = np.array([r["wobble"] for r in rows])
    mv = np.array([r["moves"] for r in rows])
    q = np.array([r["quant"] for r in rows])
    h = np.array([r["hit"] for r in rows])
    print("   %-22s wobble %5.3f   moves %4.1f   on a quarter %5.1f %%   "
          "the grid explains %5.1f %%"
          % (name, np.median(w), mv.mean(), q.mean() * 100.0,
             np.median(h) * 100.0))
    if "err" in rows[0]:
        e = np.array([r["err"] for r in rows])
        print("   %-22s err vs librosa %5.3f (90th %5.3f)"
              % ("", np.median(e), np.percentile(e, 90)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=60)
    args = ap.parse_args()
    data = []
    for k in _cache.keys()[:args.n]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = NEW.odf_of(x)
        if len(odf) < 1200:
            continue
        data.append((k, odf))
    print("== %d windows" % len(data))
    t0 = time.time()

    modes = [("the plateau's centre", dict(rate_mode="plateau")),
             ("the least-squares fit", dict(rate_mode="refine")),
             ("the fit, uniform", dict(rate_mode="refine",
                                       refine_uniform=True)),
             ("the fit, wide", dict(rate_mode="refine", refine_tol=2.0)),
             ("the fit, wide+uniform", dict(rate_mode="refine",
                                            refine_tol=2.0,
                                            refine_uniform=True))]
    for name, kw in modes:
        rows = []
        for k, odf in data:
            m = measure(odf, oracle(k), **kw)
            if m is not None:
                rows.append(m)
        report(name, rows)
    print("   %.0f s" % (time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())

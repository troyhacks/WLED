#!/usr/bin/env python3
"""A/B: the committed tracker against the one with the new rule.

The metrics are the ones that matter, and they are measured on the real
music in the cache:

  wobble   how much the published rate moves over the LAST TEN SECONDS -
           the steady state, after the tracker has settled.  This is
           Troy's flicker: "128 and 127 and back to 128".
  moves    how many times a second the published rate moves by more than a
           quarter of a BPM, in those ten seconds.
  beat ms  how far the grid's beats are from the music's onsets.  This is
           "the beat grid falls out of sync".
  settle   how long the tracker took to reach the rate it ended on.
  final    the rate it ended on, and the median of the last ten seconds.

    python tools/ar_sim/_bg_ab.py --n 100
"""

import os
import subprocess
import sys
import tempfile
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _fw_bg as NEW                                    # noqa: E402
import _fw_port as P                                   # noqa: E402
import _cache                                          # noqa: E402


def load_committed():
    """The tracker as it is in git, so the A/B is against the COMMITTED
    version and not against a copy that drifts."""
    src = subprocess.check_output(
        ["git", "show", "HEAD:tools/ar_sim/_fw_bg.py"],
        cwd=os.path.dirname(os.path.dirname(os.path.dirname(
            os.path.abspath(__file__)))))
    path = os.path.join(tempfile.mkdtemp(), "_fw_bg_committed.py")
    with open(path, "wb") as f:
        f.write(src)
    sys.path.insert(0, os.path.dirname(path))
    import importlib
    mod = importlib.import_module("_fw_bg_committed")
    return mod


def beat_error(t):
    if not t.beats:
        return 0.0
    onsets = np.array([f for f, _ in t.peak_frame], dtype=float)
    if len(onsets) == 0:
        return 0.0
    errs = [np.min(np.abs(onsets - b)) / P.RATE * 1000.0 for b in t.beats]
    return float(np.median(np.array(errs)))


def grid_hit(t):
    """How well the grid sits on the music: the best global offset and
    the fraction of the music's onsets that the grid explains.

    The naive beat error is useless here - it measures the offset of every
    beat to the nearest onset, so a grid that locked late (or by luck) can
    look better than one that is right.  This asks the other question:
    with ONE offset applied to the whole grid, how much of the music does
    it explain?
    """
    if not t.beats:
        return 0.0, 0.0
    beats = np.array(t.beats, dtype=float) / P.RATE
    onsets = np.array([f for f, _ in t.peak_frame], dtype=float) / P.RATE
    if len(onsets) == 0:
        return 0.0, 0.0
    return P.hit_with_offset(beats, onsets)


def measure(mod, odf):
    class F(mod.BgTracker):
        """The tracker with every answer written down, so the settled rate
        can be compared with the measurement it came from."""

        def __init__(self, *a, **kw):
            super().__init__(*a, **kw)
            self.answers = []

        def narrow_result(self, rate, phase, score, local):
            super().narrow_result(rate, phase, score, local)
            self.answers.append(rate)

    t = F()
    t.run(odf)
    if not t.locked or t.bpm <= 0:
        return None
    log = np.array(t.bpm_log)
    n = len(log)
    fps = int(round(P.RATE))
    tail = log[int(n - 10 * fps):]
    if len(tail) == 0:
        return None
    # the biggest step between one second and the next, in the tail
    per = [float(np.median(tail[i:i + fps]))
           for i in range(0, len(tail) - fps, fps)]
    steps = np.abs(np.diff(per)) if len(per) > 1 else np.array([0.0])
    final = float(tail[-1])
    med = float(np.median(tail))
    # the settle time: the first moment the rate was within 0.5 BPM of the
    # rate it ended on, and stayed there
    settle = -1.0
    for i in range(n):
        if abs(log[i] - final) <= 0.5:
            if np.all(np.abs(log[i:] - final) <= 0.5):
                settle = i / float(fps)
                break
    # The truth is the comb's OWN answer over the SAME window we measured
    # our rate over - the last ten seconds.  Using the median over the
    # whole window would compare the steady state against a confused
    # beginning, which is the flicker we just fixed.
    ans = np.array([a for a in t.answers if a > 0])
    tail_answers = max(1, int(10 * fps / 4))   # one narrow search every 4 frames
    truth = float(np.median(ans[-tail_answers:])) if len(ans) else 0.0
    hit, goff = grid_hit(t)
    return dict(wobble=float(tail.max() - tail.min()),
                moves=int((steps > 0.25).sum()),
                beat=beat_error(t),
                hit=hit, goff=goff,
                settle=settle,
                final=final, med=med, truth=truth,
                err=abs(med - truth),
                changes=int(t.changes))


def run(mod, name, data, quiet=False):
    rows = []
    t0 = time.time()
    for k, odf in data:
        m = measure(mod, odf)
        if m is not None:
            m["key"] = k
            rows.append(m)
    w = np.array([r["wobble"] for r in rows])
    mv = np.array([r["moves"] for r in rows])
    b = np.array([r["beat"] for r in rows])
    h = np.array([r["hit"] for r in rows])
    st = np.array([r["settle"] for r in rows])
    print("== %s   %d windows, %.0f s" % (name, len(rows), time.time() - t0))
    print("   wobble (last 10 s): median %.3f   mean %.3f   90th %.3f   "
          "max %.3f BPM" % (np.median(w), w.mean(), np.percentile(w, 90),
                            w.max()))
    print("   windows over 0.5 BPM: %d of %d   over 1 BPM: %d"
          % (int((w > 0.5).sum()), len(w), int((w > 1.0).sum())))
    print("   moves >0.25 BPM/s (last 10 s): median %d   mean %.1f   "
          "windows with any: %d" % (np.median(mv), mv.mean(),
                                    int((mv > 0).sum())))
    print("   beat error: median %.1f ms   mean %.1f ms   90th %.1f ms"
          % (np.median(b), b.mean(), np.percentile(b, 90)))
    print("   the grid explains: median %.1f %%   mean %.1f %%   10th %.1f %%"
          % (np.median(h) * 100.0, h.mean() * 100.0,
             np.percentile(h, 10) * 100.0))
    ok = st[st >= 0]
    print("   settle: median %.1f s   90th %.1f s   never: %d"
          % (np.median(ok) if len(ok) else -1,
             np.percentile(ok, 90) if len(ok) else -1,
             int((st < 0).sum())))
    er = np.array([r["err"] for r in rows])
    ch = np.array([r["changes"] for r in rows])
    print("   the rate's error against the measurement's own median: "
          "median %.3f   mean %.3f   90th %.3f BPM"
          % (np.median(er), er.mean(), np.percentile(er, 90)))
    print("   the far path took over: %d windows, mean %.1f times"
          % (int((ch > 0).sum()), ch.mean()))
    return rows


def main():
    argv = sys.argv[1:]
    n = 100
    i = 0
    while i < len(argv):
        if argv[i] == "--n":
            i += 1
            n = int(argv[i])
        i += 1
    data = []
    for k in _cache.keys()[:n]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = NEW.odf_of(x)
        if len(odf) < 1200:
            continue
        data.append((k, odf))
    old = run(load_committed(), "the committed tracker", data)
    new = run(NEW, "the new rule", data)
    # the per-window worst
    print("== the worst windows, the new rule")
    rows = sorted(new, key=lambda r: -r["wobble"])[:12]
    for r in rows:
        o = [q for q in old if q["key"] == r["key"]]
        ow = o[0]["wobble"] if o else -1.0
        om = o[0]["moves"] if o else -1
        print("   %-42s wobble %6.2f (was %6.2f)   moves %3d (was %3d)   "
              "beat %5.1f ms" % (r["key"][:42], r["wobble"], ow, r["moves"],
                                om, r["beat"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())

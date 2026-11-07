#!/usr/bin/env python3
"""The real music, the published rate, and how much it MOVES.

Troy's flicker: "128 and 127 and back to 128".  This walks the published
rate over the real corpus and measures the wander after the lock.

    python tools/ar_sim/_bg_walk.py            # the summary
    python tools/ar_sim/_bg_walk.py --one 3    # one window, second by second
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _fw_bg as B                                     # noqa: E402
import _fw_port as P                                   # noqa: E402
import _cache                                          # noqa: E402


def walk(odf):
    t = B.BgTracker()
    t.run(odf)
    return t


def stats(t):
    """The rate's wander, measured AFTER the lock."""
    log = np.array(t.bpm_log)
    if not t.locked:
        return None
    i = int(np.argmax(log > 0.0))
    tail = log[i:]
    if len(tail) == 0:
        return None
    # the biggest step between one second and the next
    steps = np.abs(np.diff(tail[::int(P.RATE)]))
    return dict(first=float(tail[0]), last=float(tail[-1]),
                lo=float(tail.min()), hi=float(tail.max()),
                wob=float(tail.max() - tail.min()),
                step=float(steps.max()) if len(steps) else 0.0,
                moves=int((steps > 0.25).sum()), secs=int(len(tail) / P.RATE),
                nudges=t.nudges, changes=t.changes,
                beat_err=beat_error(t))


def beat_error(t):
    """How far the grid's beats are from the music's onsets, in ms.

    This is THE number: "the beat grid falls out of sync".  Each beat the
    grid produced is matched to the nearest onset, and the error is the
    distance between them.  A tracker whose grid is on the music reads a
    few milliseconds; one that has lost the plot reads tens.
    """
    if not t.beats:
        return (0.0, 0.0)
    onsets = np.array([f for f, _ in t.peak_frame], dtype=float)
    if len(onsets) == 0:
        return (0.0, 0.0)
    errs = []
    for b in t.beats:
        d = np.min(np.abs(onsets - b))
        errs.append(d / P.RATE * 1000.0)     # ms
    errs = np.array(errs)
    return (float(np.median(errs)), float(np.percentile(errs, 90)))


def main():
    argv = sys.argv[1:]
    keys = _cache.keys()
    if argv and argv[0] == "--one":
        k = keys[int(argv[1])]
        odf = B.odf_of(_cache.load(k))
        t = walk(odf)
        log = np.array(t.bpm_log)
        sc = np.array(t.score_log)
        print("%s   %d frames" % (k, len(odf)))
        for s in range(int(len(log) / P.RATE)):
            i = int(s * P.RATE)
            j = int((s + 1) * P.RATE)
            print("  %3d s   bpm %8.3f   score %.3f" %
                  (s, float(np.median(log[i:j])), float(np.median(sc[i:j]))))
        print("   nudges %d  change %d  refused %d"
              % (t.nudges, t.changes, t.bg_refused))
        return 0

    print("== the corpus: the published rate's wander, after the lock")
    rows = []
    for k in keys:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.odf_of(x)
        if len(odf) < 1200:
            continue
        t = walk(odf)
        st = stats(t)
        if st is None:
            continue
        st["key"] = k
        rows.append(st)
    rows.sort(key=lambda r: -r["wob"])
    print("   %-40s %8s %8s %8s %8s %7s %6s %8s"
          % ("window", "first", "last", "lo", "hi", "wobble", "moves",
             "beat err"))
    for r in rows[:25]:
        print("   %-40s %8.2f %8.2f %8.2f %8.2f %7.2f %6d %5.0f/%-4.0f"
              % (r["key"][:40], r["first"], r["last"], r["lo"], r["hi"],
                 r["wob"], r["moves"], r["beat_err"][0], r["beat_err"][1]))
    w = np.array([r["wob"] for r in rows])
    m = np.array([r["moves"] for r in rows])
    e = np.array([r["beat_err"][0] for r in rows])
    print("   %d windows   wobble: median %.3f  mean %.3f  over 0.5 BPM: %d"
          % (len(w), np.median(w), w.mean(), int((w > 0.5).sum())))
    print("   moves over 0.25 BPM per second: median %d  mean %.1f  "
          "windows with any: %d"
          % (np.median(m), m.mean(), int((m > 0).sum())))
    print("   the grid's beat error: median %.1f ms  mean %.1f ms  "
          "90th %.1f ms" % (np.median(e), e.mean(), np.percentile(e, 90)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Does the RUN still work when the rate is a FIT?

The confirmation watches DIRECTION: the measurement has to keep going one
way before it may move the clock.  The plateau's centre is quantised, so
it is stable - it says "still" most of the time and jumps by a quarter
when it moves.  The FIT is continuous, and it moves a little every time an
onset on the edge of the kernel flips in or out.  A measurement that
jitters has no DIRECTION, and a run that keeps restarting never reaches
the count that lets the clock move.

So this asks the question the fit raises: over the real music, how often
does the run reach the count it needs, with the centre and with the fit?

    python tools/ar_sim/_bg_confirm.py --n 40
"""

import argparse
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_bg as NEW                                # noqa: E402
import _fw_port as P                               # noqa: E402


def measure(odf, **kw):
    t = NEW.BgTracker()
    for k, v in kw.items():
        setattr(t, k, v)
    t.run(odf)
    if not t.locked:
        return None
    need = t.persist
    # the run's count over the last five seconds, and how often it reached
    # the count it needed
    counts = t.conf_log if getattr(t, "conf_log", None) else []
    if not counts:
        return None
    reached = sum(1 for c in counts if c >= need) / float(len(counts))
    return dict(need=need,
                reached=reached,
                med=float(np.median(counts)),
                maxc=int(max(counts)))


def report(name, rows):
    if not rows:
        print("   %s: nothing" % name)
        return
    r = np.array([x["reached"] for x in rows])
    m = np.array([x["med"] for x in rows])
    mx = np.array([x["maxc"] for x in rows])
    print("   %-22s the run is complete %5.1f %% of the time   "
          "median count %6.1f   best %4d"
          % (name, r.mean() * 100.0, np.median(m), mx.max()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=40)
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
    for name, kw in [("the plateau's centre", dict(rate_mode="plateau")),
                     ("the least-squares fit", dict(rate_mode="refine"))]:
        rows = []
        for k, odf in data:
            m = measure(odf, **kw)
            if m is not None:
                rows.append(m)
        report(name, rows)
    print("   %.0f s" % (time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())

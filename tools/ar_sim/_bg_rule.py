#!/usr/bin/env python3
"""The rule that moves the clock: how long a run has to be, and what it buys.

Troy's complaint is a FLICKER - "128 and 127 and back to 128" - and a grid
that falls out of sync with the music.  The clock is moved by a rule, and
this measures the rule on the real music.

THE RULE IS A RUN.  The comb's answer is a MEASUREMENT; the clock is a
CLOCK.  The measurement may move the clock only after it has held its
COURSE - kept going one way, or kept still - for CONFIRM_N searches.  A
reversal starts the run again from there.  The narrow search runs every
four frames, so 116 searches is five seconds.

So the one number to tune is CONFIRM_N, and this sweeps it:

    python tools/ar_sim/_bg_rule.py --n 40

for each value it prints the flicker (how much the published rate moves in
the steady state), how long the tracker takes to settle, and how far the
rate it ends on is from the measurement it came from.
"""

import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _fw_bg as B                                     # noqa: E402
import _fw_port as P                                   # noqa: E402
import _cache                                          # noqa: E402


def measure(mod, odf, persist):
    class F(mod.BgTracker):
        """The tracker with every answer written down, and with the run's
        length forced, so the sweep measures the RULE and nothing else."""

        def __init__(self, *a, **kw):
            super().__init__(*a, **kw)
            self.persist = persist
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
    per = [float(np.median(tail[i:i + fps]))
           for i in range(0, len(tail) - fps, fps)]
    steps = np.abs(np.diff(per)) if len(per) > 1 else np.array([0.0])
    final = float(tail[-1])
    # the settle time: the first moment the rate was within 0.5 BPM of the
    # rate it ended on, and stayed there
    settle = -1.0
    for i in range(n):
        if abs(log[i] - final) <= 0.5:
            if np.all(np.abs(log[i:] - final) <= 0.5):
                settle = i / float(fps)
                break
    # the truth: the measurement's own median over the SAME window
    ans = np.array([a for a in t.answers if a > 0])
    tail_answers = max(1, int(10 * fps / 4))
    truth = float(np.median(ans[-tail_answers:])) if len(ans) else 0.0
    return dict(wobble=float(tail.max() - tail.min()),
                moves=int((steps > 0.25).sum()),
                settle=settle,
                err=abs(float(np.median(tail)) - truth),
                changes=int(t.changes))


def sweep(data, persist, name):
    rows = []
    for _k, odf in data:
        m = measure(B, odf, persist)
        if m is not None:
            rows.append(m)
    w = np.array([r["wobble"] for r in rows])
    mv = np.array([r["moves"] for r in rows])
    er = np.array([r["err"] for r in rows])
    st = np.array([r["settle"] for r in rows])
    ok = st[st >= 0]
    print("  %-22s wobble mean %6.3f  90th %6.3f  over 0.5: %2d/%d   "
          "moves %2d   err mean %5.3f   settle %5.1f s"
          % (name, w.mean(), np.percentile(w, 90), int((w > 0.5).sum()),
             len(w), int((mv > 0).sum()), er.mean(),
             np.median(ok) if len(ok) else -1))
    return rows


def main():
    argv = sys.argv[1:]
    n = 40
    i = 0
    values = [0, 12, 29, 58, 87, 116, 145, 232]
    while i < len(argv):
        if argv[i] == "--n":
            i += 1
            n = int(argv[i])
        elif argv[i] == "--values":
            i += 1
            values = [int(x) for x in argv[i].split(",")]
        i += 1
    data = []
    for k in _cache.keys()[:n]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.odf_of(x)
        if len(odf) < 1200:
            continue
        data.append((k, odf))
    print("%d windows" % len(data))
    t0 = time.time()
    for v in values:
        sweep(data, v, "%d searches (%.1f s)"
              % (v, v * 4.0 / P.RATE))
    print("  %.0f s" % (time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""REVIEW CHECK 7, ATTRIBUTION: whose step is the drift?

The refusal runs show the grid drifting a long way while the veto is
REFUSING (up to 12.66 BPM over 32.86 s).  The reviewer blames the NARROW
search: it has no evidence test, its window is re-centred on the grid, so
it walks the grid unchecked.

But a refused WHOLE-BAND search also moves the grid: when the veto refuses,
the sweep adopts the best rate INSIDE the window (the local best) - see
ar_tempo.h 1060-1067.  So both paths move the grid during a refusal.

So: FREEZE the narrow path and measure the drift again.

  A  the firmware, as it is
  B  the narrow search's adopt is a NO-OP: the grid cannot move on a narrow
     search at all.  A refused whole-band search still adopts the local
     best.
  C  the narrow search's adopt is a no-op AND a refused whole-band search
     does not adopt either - the grid is completely frozen during a
     refusal.  This is the upper bound of what the veto could hold.

If the drift in B is as big as in A, the narrow path is not the cause and
the finding is misattributed.  If B holds the grid and C does not, the
drift is the refusal path's own doing, not the narrow path's.

    python tools/ar_sim/_rev_attr.py --pairs 12
"""

import argparse
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402

NARROW = 2.0
EVERY = 4
NET = 32
MARGIN = 0.20
GAIN = 0.10


class Probe(P.Tracker):
    """Can freeze the narrow path, and/or the refusal path."""

    def __init__(self, freeze_narrow=False, freeze_refusal=False):
        super().__init__()
        self.freeze_narrow = freeze_narrow
        self.freeze_refusal = freeze_refusal

    def sweep_start(self):
        self._narrow = (self.locked and not self.need_full
                        and self.narrow_bpm > 0.0)
        super().sweep_start()

    def sweep_step(self):
        pre = (self.period, self.bpm, self.next_beat)
        super().sweep_step()
        if self.freeze_narrow and self._narrow:
            # the narrow search's adopt is undone
            self.period, self.bpm, self.next_beat = pre
        elif self.freeze_refusal and not self._narrow and not self.sweep_took_far:
            # a REFUSED whole-band search does not adopt either
            self.period, self.bpm, self.next_beat = pre


def make(freeze_narrow=False, freeze_refusal=False):
    t = Probe(freeze_narrow, freeze_refusal)
    t.narrow_bpm = NARROW
    t.search_every = EVERY
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = NET
    t.net_margin = MARGIN
    t.veto_on_collapse = True
    t.pll_gain = GAIN
    t.exact_kernel = True
    return t


def reference_bpm(odf):
    t = P.Tracker()
    t.narrow_bpm = 0.0
    t.run(odf)
    return t.bpm if t.locked and t.bpm > 0 else None


def load_odfs(windows):
    cache_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "_rev_odf_cache")
    os.makedirs(cache_dir, exist_ok=True)
    items = []
    for k in _cache.keys()[:windows]:
        x = _cache.load(k)
        if x is None:
            continue
        cpath = os.path.join(cache_dir, k.replace("/", "_") + ".npy")
        if os.path.exists(cpath):
            odf = np.load(cpath)
        else:
            odf = B.firmware_odf(x, 80.0, 320.0)
            np.save(cpath, odf)
        if len(odf) < 1200:
            continue
        items.append((k, odf))
    return items


def refusal_drift(t, cut, log):
    dec = [r for r in t.trace if r["frame"] >= cut]
    worst = 0.0
    txt = ""
    run = []
    runs = []

    def close(run):
        if not run:
            return
        f0 = max(cut, run[0]["frame"])
        f1 = min(len(log), run[-1]["frame"] + 34)
        if f1 > f0:
            runs.append((float(log[f1 - 1] - log[f0]), (f1 - f0) / P.RATE,
                         float(log[f0]), float(log[f1 - 1])))
    for r in dec:
        if r["took_far"]:
            close(run)
            run = []
            continue
        run.append(r)
    close(run)
    if runs:
        worst = max(runs, key=lambda z: abs(z[0]))
        txt = "%+.2f BPM over %.2f s (%.2f -> %.2f)" % (worst[0], worst[1],
                                                       worst[2], worst[3])
    return worst[0] if runs else 0.0, txt, len(runs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=30)
    ap.add_argument("--pairs", type=int, default=12)
    ap.add_argument("--gap", type=float, default=8.0)
    args = ap.parse_args()

    t0 = time.time()
    items = load_odfs(args.windows)
    refs = {}
    for (k, odf) in items:
        refs[k] = reference_bpm(odf)
    pairs = []
    for i in range(len(items)):
        a = refs.get(items[i][0])
        if a is None:
            continue
        for j in range(len(items)):
            if j == i:
                continue
            b = refs.get(items[j][0])
            if b is None or abs(b - a) < args.gap:
                continue
            pairs.append((items[i], items[j], b))
            break
    pairs = pairs[:args.pairs]
    print("%d pairs   %.0f s" % (len(pairs), time.time() - t0), flush=True)
    print()

    print("=" * 104)
    print("A  THE FIRMWARE")
    print("=" * 104)
    print("%-42s %10s %10s %9s %9s"
          % ("pair", "drift", "dur s", "reached", "final"))
    res = {}
    for name, (fn, fr) in (("A", (False, False)),
                           ("B", (True, False)),
                           ("C", (True, True))):
        worst_all = 0.0
        txt = ""
        nruns = 0
        reached = 0
        rows = []
        for (a, b, ref) in pairs:
            t = make(fn, fr)
            cut = len(a[1])
            t.run(np.concatenate((a[1], b[1])))
            log = np.array(t.bpm_log)
            d, dtxt, nr = refusal_drift(t, cut, log)
            nruns += nr
            hit = None
            for f in range(cut, len(log)):
                if abs(log[f] - ref) <= 1.0:
                    hit = (f - cut) / P.RATE
                    break
            reached += 1 if hit is not None else 0
            rows.append((a[0][:17], b[0][:17], d, dtxt,
                         ("%.1f s" % hit) if hit is not None else "NO", log[-1]))
            if abs(d) > abs(worst_all):
                worst_all = d
                txt = "%s -> %s: %s" % (a[0][:17], b[0][:17], dtxt)
        res[name] = (worst_all, txt, reached, nruns)
        if name == "A":
            print("%-42s %10s %10s %9s %9s"
                  % ("pair", "drift", "dur s", "reached", "final"))
            for r in rows:
                print("%-42s %+10.2f %10s %9s %9.2f"
                      % ("%s -> %s" % (r[0], r[1]), r[2], r[3].split(" over ")[1]
                         if " over " in r[3] else "-", r[4], r[5]))
        print()
        print("%s  the biggest drift in a refusal run: %+.2f BPM   (%s)"
              % (name, worst_all, txt))
        print("%s  the grid reached the new rate in %d of %d pairs, over %d "
              "refusal runs" % (name, reached, len(pairs), nruns))
        print(flush=True)

    print("=" * 104)
    print("A  the firmware           : %+.2f BPM  biggest refusal-run drift"
          % res["A"][0])
    print("B  the narrow path frozen : %+.2f BPM" % res["B"][0])
    print("C  frozen completely      : %+.2f BPM" % res["C"][0])
    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

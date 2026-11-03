#!/usr/bin/env python3
"""THE NARROW WALK, MEASURED, AT THE SHIPPED CONFIGURATION.

The reviewer claims the narrow path walks the grid across a gap at up to
4.7 BPM/s, crossing 6 BPM in about 30 adopts (1.3 s), and that the walk
never stops because the window is re-centred on the grid every 4 frames.

This measures, per narrow adopt:
  - the step in BPM,
  - the SUSTAINED rate (total narrow travel / total time),
  - how long the grid took to cross the gap and get within 1 BPM of the
    new track's rate,
and it counts the adopts, so the reviewer's "30 adopts" is checkable.
"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache, _fw_band as B, _fw_port as P

HERE = os.path.dirname(os.path.abspath(__file__))
cd = os.path.join(HERE, "_rev_odf_cache")


class Probe(P.Tracker):
    def sweep_start(self):
        self._narrow = (self.locked and not self.need_full and self.narrow_bpm > 0.0)
        super().sweep_start()
    def sweep_step(self):
        before = self.bpm
        super().sweep_step()
        if self.bpm != before:
            self._adopts.append((self.frame_index, before, self.bpm, self._narrow))


def load(windows):
    items = []
    for k in _cache.keys()[:windows]:
        x = _cache.load(k)
        if x is None:
            continue
        cp = os.path.join(cd, k.replace("/", "_") + ".npy")
        if os.path.exists(cp):
            odf = np.load(cp)
        else:
            odf = B.firmware_odf(x, 80.0, 320.0)
            np.save(cp, odf)
        if len(odf) >= 1200:
            items.append((k, odf))
    return items


def reference_bpm(odf):
    t = P.Tracker(); t.narrow_bpm = 0.0; t.run(odf)
    return t.bpm if t.locked and t.bpm > 0 else None


def main():
    items = load(30)
    refs = {k: reference_bpm(o) for (k, o) in items}
    pairs = []
    for i in range(len(items)):
        a = refs.get(items[i][0])
        if a is None:
            continue
        for j in range(len(items)):
            if j == i:
                continue
            b = refs.get(items[j][0])
            if b is None or abs(b - a) < 8.0:
                continue
            pairs.append((items[i], items[j], b)); break
    pairs = pairs[:14]
    print("%d pairs" % len(pairs))
    print("%-40s %8s %8s %7s %8s %8s" % ("pair", "gap", "cross s", "adopts", "travel", "BPM/s"))
    steps = []
    sustained = []
    cross_times = []
    for (a, b, ref) in pairs:
        t = Probe(); t._adopts = []
        cut = len(a[1])
        t.run(np.concatenate((a[1], b[1])))
        # the grid before the cut
        pre = None
        for (f, old, new, nw) in t._adopts:
            if f < cut:
                continue
            steps.append(abs(new - old))
            if nw:
                sustained.append((f, new - old))
        # the grid's value just before the cut
        g0 = None
        for (f, old, new, nw) in t._adopts:
            if f < cut:
                g0 = new
        if g0 is None:
            continue
        gap = abs(ref - g0)
        # the time it took to arrive within 1 BPM of the new rate
        arrive = None
        for (f, old, new, nw) in t._adopts:
            if f >= cut and abs(new - ref) <= 1.0:
                arrive = f; break
        # the narrow travel after the cut
        trav = 0.0
        t0 = None
        for (f, old, new, nw) in t._adopts:
            if f < cut:
                continue
            if nw:
                if t0 is None:
                    t0 = f
                trav += new - old
        dur = (t._adopts[-1][0] - cut) / P.RATE if t._adopts else 0.0
        print("%-40s %8.2f %8s %7s %8.2f %8.2f" % (
            ("%s -> %s" % (a[0][:16], b[0][:16])),
            gap,
            ("%.2f" % ((arrive - cut) / P.RATE)) if arrive else "NO",
            len([1 for (f, o, n, nw) in t._adopts if f >= cut and nw]),
            trav,
            (trav / dur) if dur > 0 else 0.0))
        if arrive:
            cross_times.append((arrive - cut) / P.RATE)
    s = np.array(steps)
    print()
    print("every adopt's step in BPM: n %d  median %.4f  mean %.4f  p99 %.4f  MAX %.4f"
          % (len(s), np.median(s), s.mean(), np.percentile(s, 99), s.max()))
    print("  the reviewer's per-adopt ceiling of 0.2 BPM: %d of %d adopts exceeded it"
          % ((s > 0.2).sum(), len(s)))
    print("  the reviewer's claimed sustained rate 4.7 BPM/s; the mean per-adopt")
    print("    rate needed for that is %.4f BPM x %d adopts/s" % (0.2, 23.4))
    if cross_times:
        print("  time to cross to within 1 BPM of the new rate: mean %.2f s, max %.2f s"
              % (float(np.mean(cross_times)), max(cross_times)))


main()

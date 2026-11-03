#!/usr/bin/env python3
"""THE REVIEWER'S OWN EXPERIMENT.

'with the whole-band search removed entirely (full_every = 0, collapse
off), 8 of 14 track changes still carried the grid across the gap'

This runs it against the CURRENT build (gain 0.20, margin 0.70), and
reports which path did the carrying.
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

    for label, kw in (("FULL BUILD (whole-band search on)", {}),
                      ("NO WHOLE-BAND SEARCH (full_every=0, collapse off)",
                       dict(full_every=0, collapse_trigger=False))):
        carried = 0
        by_narrow = 0
        print()
        print(label)
        for (a, b, ref) in pairs:
            t = Probe(); t._adopts = []
            t.narrow_bpm = 2.0; t.search_every = 4; t.edge_rule = False
            t.collapse_trigger = True; t.full_every = 32
            t.net_margin = 0.70; t.veto_on_collapse = True
            t.exact_kernel = True
            for k, v in kw.items():
                setattr(t, k, v)
            cut = len(a[1])
            t.run(np.concatenate((a[1], b[1])))
            g0 = None
            for (f, old, new, nw) in t._adopts:
                if f < cut:
                    g0 = new
            if g0 is None:
                continue
            gap = abs(ref - g0)
            # the grid's extreme after the cut
            tail = np.array(t.bpm_log)[cut:]
            crossed = (np.any(tail < ref - gap * 0.5) and np.any(tail > ref + gap * 0.5)) \
                or (abs(tail[-1] - ref) < gap * 0.5)
            # "carried across the gap": the grid ended up near the NEW rate
            # having started near the old one
            end = tail[-1]
            moved = abs(end - g0)
            frac = moved / gap if gap else 0.0
            if frac >= 0.5:
                carried += 1
                if not t._adopts or True:
                    pass
            # which path moved it
            trav_n = 0.0
            prev = g0
            for (f, old, new, nw) in t._adopts:
                if f < cut:
                    continue
                if nw:
                    trav_n += new - old
                prev = new
            if frac >= 0.5 and abs(trav_n) > abs(moved) * 0.5:
                by_narrow += 1
            print("   %-40s gap %6.2f  old %7.2f  new %7.2f  end %7.2f  "
                  "moved %5.2f (%3.0f%%)  narrow travel %+6.2f"
                  % (("%s -> %s" % (a[0][:16], b[0][:16])), gap, g0, ref, end,
                     moved, 100 * frac, trav_n))
        print("   CARRIED ACROSS THE GAP (moved >= 50%% of the gap): %d of %d"
              % (carried, len(pairs)))
        print("   of those, the NARROW path did most of the moving: %d" % by_narrow)


main()

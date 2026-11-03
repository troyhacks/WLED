#!/usr/bin/env python3
"""TRACE THE VETO, PRE-VETO, WITH THE FIRMWARE'S BAND AND PLL.

For every whole-band search it recomputes what the far winner and the narrow
winner were BEFORE the veto ran, and prints their raw scores and the ratio.
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_band as B                               # noqa: E402
import _fw_port as P                               # noqa: E402


class Traced(P.Tracker):
    def __init__(self):
        super().__init__()
        self.events = []
        self._raw = None
        self._bpm0 = 0.0

    def comb_scores_all(self, frames, strengths, bpms):
        s, p = super().comb_scores_all(frames, strengths, bpms)
        self._raw = (np.array(bpms, dtype=float), np.array(s, dtype=float),
                     np.array(p, dtype=float))
        return s, p

    def sweep_start(self):
        self._bpm0 = self.bpm
        self._raw = None
        super().sweep_start()
        if self._raw is None or self.sweep_was_narrow:
            return
        bpms, scores, phases = self._raw
        if len(bpms) != P.CAND_COUNT:
            return
        values = scores - self.prior
        jfar = int(np.argmax(values))
        f, c = P.narrow_window(self._bpm0, self.narrow_bpm)
        idx = np.arange(f, f + c)
        jnar = int(idx[np.argmax(scores[idx])])
        inside = (f <= jfar <= f + c - 1)
        sf = float(scores[jfar])
        sn = float(scores[jnar])
        alive = sn >= P.MIN_SCORE
        vetoed = (not inside) and alive and sf < sn * (1.0 + self.net_margin)
        self.events.append(dict(
            frame=self.frame_index,
            grid=self._bpm0,
            far=float(bpms[jfar]), fars=sf,
            nar=float(bpms[jnar]), nars=sn,
            ratio=(sf / sn) if sn > 0 else 0.0,
            inside=inside, alive=alive, vetoed=vetoed,
            farvalue=float(values[jfar]),
        ))


def reference_bpm(odf):
    t = P.Tracker()
    t.narrow_bpm = 0.0
    t.run(odf)
    return t.bpm if t.locked and t.bpm > 0 else None


def make(margin, gain=0.10):
    t = Traced()
    t.narrow_bpm = 2.0
    t.search_every = 4
    t.edge_rule = False
    t.collapse_trigger = True
    t.full_every = 32
    t.net_margin = margin
    t.pll_gain = gain
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=30)
    ap.add_argument("--pairs", type=int, default=12)
    ap.add_argument("--lo", type=float, default=40.0)
    ap.add_argument("--hi", type=float, default=160.0)
    ap.add_argument("--margin", type=float, default=0.20)
    ap.add_argument("--show", type=int, default=4)
    args = ap.parse_args()

    items = []
    for k in _cache.keys()[:args.windows]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = B.firmware_odf(x, args.lo, args.hi)
        if len(odf) < 1200:
            continue
        items.append((k, odf))
    pairs = []
    for i in range(len(items)):
        bpm = reference_bpm(items[i][1])
        if bpm is None:
            continue
        for j in range(i + 1, len(items)):
            bpm2 = reference_bpm(items[j][1])
            if bpm2 is None or abs(bpm2 - bpm) < 6.0:
                continue
            pairs.append((items[i], items[j], bpm2))
            break
        if len(pairs) >= args.pairs:
            break

    shown = 0
    for (a, b, ref) in pairs:
        t = make(args.margin)
        cut = len(a[1])
        t.run(np.concatenate((a[1], b[1])))
        log = np.array(t.bpm_log)
        # when does it first sit at the reference?
        got = [f for f in range(cut, len(log)) if t.locked and abs(log[f] - ref) <= 1.0]
        first = (got[0] - cut) / P.RATE if got else None
        print("\n%s -> %s   truth %.2f   reached %s   end %.2f   score %.3f"
              % (a[0][:30], b[0][:30], ref,
                 ("%.2f s" % first) if first is not None else "NEVER",
                 t.bpm, t.score))
        nv = sum(1 for e in t.events if e["vetoed"])
        print("   whole-band searches %d   vetoed %d   net_far %d"
              % (len(t.events), nv, t.net_far))
        if shown < args.show and first is None:
            shown += 1
            print("   %-7s %8s %8s %8s %8s %8s %7s %6s"
                  % ("frame", "grid", "far", "farsc", "narrow", "narsc",
                     "ratio", "veto?"))
            for e in t.events:
                if e["frame"] < cut - 100:
                    continue
                print("   %-7d %8.2f %8.2f %8.3f %8.2f %8.3f %7.2f %6s"
                      % (e["frame"], e["grid"], e["far"], e["fars"], e["nar"],
                         e["nars"], e["ratio"], e["vetoed"]))


if __name__ == "__main__":
    sys.exit(main())

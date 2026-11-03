#!/usr/bin/env python3
"""REVIEW CHECK 2: does the veto's refusal HOLD?

The reviewer's claim, restated as a testable question:

  The veto refuses a far whole-band winner unless it is 20 % better.
  But the narrow search has NO evidence test at all, and its window is
  re-centred on the grid every four frames, so the narrow path can walk
  the grid onto a rate the veto just refused.

So: for every track change, does the grid reach a rate that a VETOED far
winner nominated?

For each pair (A then B) in the FIRMWARE configuration this prints:
  - the grid's rate at the cut, and B's reference rate
  - every veto decision after the cut: the near rate and score, the far
    rate and score, the ratio, and whether the far was TAKEN or REFUSED
  - the grid's trajectory after the cut
  - the verdict: did the grid arrive at a rate that some veto had refused?
    and if so, how long after that refusal?

Also: the same run with the reviewer's proposed FIX applied - the grid is
not allowed to drift more than AR_TEMPO_NARROW_BPM from the last rate a
veto APPROVED - to see whether the fix changes the outcome.

    python tools/ar_sim/_rev_veto_hold.py --pairs 12
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
    """Records the veto's decisions and every adopt."""

    def __init__(self, drift_cap=None):
        super().__init__()
        self.trace = []            # the veto's own record (the port's)
        self.adopts = []           # (frame, bpm, was_narrow)
        self.approved = None       # the last rate a veto APPROVED
        self.drift_cap = drift_cap
        self.blocked = 0

    def sweep_start(self):
        self._pre_far = self.net_far
        self._pre_veto = self.net_vetoes
        self._narrow = (self.locked and not self.need_full
                        and self.narrow_bpm > 0.0)
        super().sweep_start()

    def sweep_step(self):
        before = self.bpm
        super().sweep_step()
        if self.bpm != before:
            self.adopts.append((self.frame_index, self.bpm, self._narrow))
        # the veto's own record
        if self.net_vetoes > self._pre_veto or self.net_far > self._pre_far:
            rec = self.trace[-1] if self.trace else None
            if rec is not None:
                rec["took_far"] = bool(self.net_far > self._pre_far)

    def adopt(self, bpm, phase, now):
        """The reviewer's FIX, optionally: cap the grid's drift."""
        if self.drift_cap is not None and self.locked and self.approved is not None:
            if abs(bpm - self.approved) > self.drift_cap:
                self.blocked += 1
                bpm = self.approved
        super().adopt(bpm, phase, now)


def firmware_tracker(drift_cap=None):
    t = Probe(drift_cap=drift_cap)
    t.narrow_bpm = NARROW
    t.search_every = EVERY
    t.edge_rule = False            # the C++ has no edge rule
    t.collapse_trigger = True      # the C++ has the collapse path
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=30)
    ap.add_argument("--pairs", type=int, default=12)
    ap.add_argument("--gap", type=float, default=8.0)
    args = ap.parse_args()

    t0 = time.time()
    items = load_odfs(args.windows)
    print("%d windows   %.0f s" % (len(items), time.time() - t0), flush=True)

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
    print("THE FIRMWARE CONFIGURATION.  Does a VETOED far rate still end up")
    print("on the grid, by the narrow path?")
    print("=" * 104)
    print("%-40s %8s %8s %7s %9s %9s %9s %9s"
          % ("pair", "A", "B", "reach", "refusals", "unwound", "walk s", "veto"))
    reached = 0
    unwound = 0
    for (a, b, ref) in pairs:
        t = firmware_tracker()
        cut = len(a[1])
        t.run(np.concatenate((a[1], b[1])))
        log = np.array(t.bpm_log)

        # the veto's decisions after the cut
        refusals = [r for r in t.trace if r["frame"] >= cut]
        refused_far = [r for r in refusals if not r.get("took_far")]

        # did the grid arrive at a rate a refusal had nominated?
        unwound_at = None
        for r in refused_far:
            fb = r["far_bpm"]
            if fb <= 0:
                continue
            for f in range(max(cut, r["frame"]), len(log)):
                if abs(log[f] - fb) <= 0.5:
                    unwound_at = (f - cut) / P.RATE
                    break
            if unwound_at is not None:
                break

        # when did the grid reach B's rate?
        hit = None
        for f in range(cut, len(log)):
            if abs(log[f] - ref) <= 1.0:
                hit = (f - cut) / P.RATE
                break

        reached += 1 if hit is not None else 0
        unwound += 1 if unwound_at is not None else 0
        print("%-40s %8.2f %8.2f %7s %9d %9s %9s %9d"
              % (("%s -> %s" % (a[0][:17], b[0][:17])),
                 log[cut - 1] if cut else 0.0, ref,
                 ("%.1f s" % hit) if hit is not None else "NO",
                 len(refused_far),
                 ("%.1f s" % unwound_at) if unwound_at is not None else "-",
                 "-", len(refusals)),
              flush=True)
    print()
    print("reached B's rate in %d of %d pairs" % (reached, len(pairs)))
    print("the grid landed on a REFUSED far rate in %d of %d pairs"
          % (unwound, len(pairs)))
    print()

    # ---- the reviewer's fix, applied -------------------------------------
    print("=" * 104)
    print("WITH THE REVIEWER'S FIX: the grid may not drift more than")
    print("AR_TEMPO_NARROW_BPM (%.1f) from the last APPROVED rate" % NARROW)
    print("=" * 104)
    reached2 = 0
    unwound2 = 0
    for (a, b, ref) in pairs:
        t = firmware_tracker(drift_cap=NARROW)
        cut = len(a[1])
        t.run(np.concatenate((a[1], b[1])))
        log = np.array(t.bpm_log)
        refusals = [r for r in t.trace if r["frame"] >= cut]
        refused_far = [r for r in refusals if not r.get("took_far")]
        unwound_at = None
        for r in refused_far:
            fb = r["far_bpm"]
            if fb <= 0:
                continue
            for f in range(max(cut, r["frame"]), len(log)):
                if abs(log[f] - fb) <= 0.5:
                    unwound_at = (f - cut) / P.RATE
                    break
            if unwound_at is not None:
                break
        hit = None
        for f in range(cut, len(log)):
            if abs(log[f] - ref) <= 1.0:
                hit = (f - cut) / P.RATE
                break
        reached2 += 1 if hit is not None else 0
        unwound2 += 1 if unwound_at is not None else 0
        print("%-40s %8.2f %8.2f %7s %9s %9s %9d"
              % (("%s -> %s" % (a[0][:17], b[0][:17])),
                 log[cut - 1] if cut else 0.0, ref,
                 ("%.1f s" % hit) if hit is not None else "NO",
                 "-",
                 ("%.1f s" % unwound_at) if unwound_at is not None else "-",
                 t.blocked),
              flush=True)
    print()
    print("reached B's rate in %d of %d pairs with the fix" % (reached2, len(pairs)))
    print("landed on a REFUSED far rate in %d of %d pairs with the fix"
          % (unwound2, len(pairs)))
    print()
    print("%.0f s" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())

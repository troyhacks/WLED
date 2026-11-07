#!/usr/bin/env python3
"""The corpus, against the ORACLE: which tracker is closer to the truth?

The reference in _fw_bg.py's corpus case is "the rate the same tracker finds
when it is given the window on its own" - which is a tracker, not a truth.
This compares both trackers against librosa's independent beat tracker.

    python tools/ar_sim/_bg_corpus.py [n]
"""

import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _fw_bg as B                                 # noqa: E402
import _fw_port as P                               # noqa: E402
import _oracle_librosa as O                        # noqa: E402
from _fw_band import firmware_odf                  # noqa: E402


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 60
    t0 = time.time()
    rows = []
    for k in _cache.keys()[:n]:
        x = _cache.load(k)
        if x is None:
            continue
        odf = firmware_odf(x, P.BAND_LO, P.BAND_HI)
        if len(odf) < 1200:
            continue
        orc = O.load(k)
        if orc is None or not orc.get("bpm"):
            continue
        truth = float(orc["bpm"])
        # the OLD tracker: the one in the firmware today.
        old = P.Tracker()
        old.quant = 1
        old.whole_bonus = 0.01
        old.run(odf)
        # the NEW tracker: two estimators, one grid.
        new = B.BgTracker()
        new.run(odf)
        rows.append((k, truth, old.bpm, new.bpm,
                     old.locked, new.locked))
        print("  %-46s truth %7.2f  old %7.2f  new %7.2f"
              % (k[:46], truth, old.bpm, new.bpm), flush=True)
    if not rows:
        print("no windows")
        return
    tr = np.array([r[1] for r in rows])
    od = np.array([r[2] for r in rows])
    nw = np.array([r[3] for r in rows])
    eo = np.abs(od - tr)
    en = np.abs(nw - tr)
    print()
    print("  n = %d   %.0f s" % (len(rows), time.time() - t0))
    print("  OLD  median %.3f  mean %.3f  p90 %.3f  hit(0.5) %.2f  "
          "hit(1.0) %.2f"
          % (np.median(eo), eo.mean(), np.percentile(eo, 90),
             (eo <= 0.5).mean(), (eo <= 1.0).mean()))
    print("  NEW  median %.3f  mean %.3f  p90 %.3f  hit(0.5) %.2f  "
          "hit(1.0) %.2f"
          % (np.median(en), en.mean(), np.percentile(en, 90),
             (en <= 0.5).mean(), (en <= 1.0).mean()))
    worse = np.nonzero(en > eo + 0.5)[0]
    better = np.nonzero(eo > en + 0.5)[0]
    print("  the new one is worse on %d, better on %d" % (len(worse), len(better)))
    for i in worse[:12]:
        print("    WORSE %-42s truth %7.2f  old %7.2f  new %7.2f"
              % (rows[i][0][:42], rows[i][1], rows[i][2], rows[i][3]))
    for i in better[:12]:
        print("    BETTER %-42s truth %7.2f  old %7.2f  new %7.2f"
              % (rows[i][0][:42], rows[i][1], rows[i][2], rows[i][3]))


if __name__ == "__main__":
    main()

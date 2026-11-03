#!/usr/bin/env python3
"""What metrical level is the ORACLE at - and what level is right?

The Daft Punk window shows the shape of the problem.  The autocorrelation
has TWO peaks:

    lag 35  ->  160.7 BPM   r = 632   (tallest)
    lag 47  ->  119.7 BPM   r = 359

and librosa says the tempo is 122.28.  So the beat is at lag 46-47 and the
ACF's tallest peak is somewhere else entirely.  A peak-picker that takes
the tallest peak is wrong here.

But WHY is lag 35 tall?  Two candidates:

(a) The music has a real subdivision at 160 BPM that the ODF sees more
    strongly than the beat.  Then a comb-filter score - which asks "do
    the ODF's ONSETS land on this grid" - would pick the beat, because the
    subdivision's onset pattern is not evenly spaced on the beat grid.

(b) librosa is at the EIGHTH-note level on some windows and the QUARTER
    level on others.  Then "aligning to librosa" is measuring the oracle's
    inconsistency, and the estimator is being blamed for it.

This script separates them.  For each window it asks: does the ODF have
significant energy at the MIDPOINTS between librosa's beats?  If yes,
librosa is at the faster level and its "tempo" is twice the beat.  If no,
librosa is at the beat level and the estimator is simply wrong.

The answer decides the architecture: if it is (a), the fix is to score ACF
candidates with a comb filter instead of by height.  If it is (b), the fix
is to fix the oracle.

    python tools/ar_sim/_diag_level.py
"""

import argparse
import os
import sys
from collections import defaultdict

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402
import _oracle_librosa                             # noqa: E402
import _tempo_acf as T                             # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--design", default="hp40_lp160x2")
    ap.add_argument("--whiten", type=float, default=0.0)
    ap.add_argument("--show", type=int, default=20)
    args = ap.parse_args()

    # For each window: sample the ODF at librosa's beat times, and at the
    # midpoints between them.  If the midpoints are as strong as the beats,
    # the level is ambiguous or the oracle is at the faster level.
    beat_vals = []
    mid_vals = []
    rows = []
    for k in _cache.keys():
        o = _oracle_librosa.load(k)
        x = _cache.load(k)
        if o is None or x is None or not o.get("beats"):
            continue
        odf = T.odf_cached(k, x, args.design, args.whiten)
        bt = np.asarray(o["beats"], dtype=float)
        ibi = np.diff(bt)
        med_ibi = float(np.median(ibi)) if len(ibi) else 0.0
        if med_ibi <= 0:
            continue
        # the ODF frame index of each beat, relative to the window start
        # the cached window is 40 s of audio; librosa's beats are seconds
        # from the window start, so frame = t * RATE
        bvals = []
        mvals = []
        for i, t in enumerate(bt):
            fi = int(round(t * T.RATE))
            if 0 <= fi < len(odf):
                bvals.append(odf[fi])
            # midpoint to the next beat
            if i + 1 < len(bt):
                fm = int(round((t + ibi[i] * 0.5) * T.RATE))
                if 0 <= fm < len(odf):
                    mvals.append(odf[fm])
        if not bvals or not mvals:
            continue
        b = float(np.mean(bvals))
        m = float(np.mean(mvals))
        ratio = m / b if b > 0 else 0.0
        rows.append((k, ratio, b, m, med_ibi, o["bpm"]))
        beat_vals.append(b)
        mid_vals.append(m)

    print("%d windows" % len(rows))
    ratios = np.array([r[1] for r in rows])
    print()
    print("the ODF's value at the MIDPOINT between librosa's beats,")
    print("as a fraction of the ODF's value AT librosa's beats:")
    print("  <0.30 (librosa is at the beat level, the midpoints are empty): %5.1f%%"
          % (100.0 * float((ratios < 0.30).mean())))
    print("  0.30-0.70 (ambiguous - the midpoints are half as strong)    : %5.1f%%"
          % (100.0 * float(((ratios >= 0.30) & (ratios < 0.70)).mean())))
    print("  >0.70 (librosa is at the faster level, the midpoints are as strong): %5.1f%%"
          % (100.0 * float((ratios > 0.70).mean())))

    # per-track: does the ratio change between windows of the same track?
    per = defaultdict(list)
    for k, ratio, b, m, med_ibi, lib in rows:
        per[k.split("@")[0]].append(ratio)
    varying = sum(1 for v in per.values()
                   if len(v) > 1 and (max(v) - min(v)) > 0.4)
    print()
    print("%d of %d tracks have the midpoint ratio move by more than 0.4"
          % (varying, len(per)))
    print("between their own windows - i.e. librosa changes metrical level")
    print("within a track as often as it does between tracks.")

    # Now: the ACF's peak, and the ACF peak that a COMB FILTER would pick.
    # If the comb filter does much better, the fix is (a).
    print()
    print("for the windows where the ACF's tallest peak is NOT the beat:")
    print("  acf peak     comb peak    librosa    what the comb found")
    bad = 0
    shown = 0
    for k, ratio, b, m, med_ibi, lib in rows:
        x = _cache.load(k)
        o = _oracle_librosa.load(k)
        odf = T.odf_cached(k, x, args.design, args.whiten)
        lags, r = T.acf_of(odf, T.WIN)
        acf_bpm, _c, _g = T.estimate_acf(odf, win=T.WIN)
        comb_bpm, _s = T.estimate_comb(odf, T.WIN)
        if acf_bpm is None or comb_bpm is None:
            continue
        if abs(acf_bpm - lib) < 3.0:
            continue
        bad += 1
        if shown < args.show:
            shown += 1
            print("  %7.2f     %7.2f   %7.2f   ratio acf/lib %.3f  comb/lib %.3f"
                  % (acf_bpm, comb_bpm, lib, acf_bpm / lib, comb_bpm / lib))
    print()
    print("%d of %d windows have the ACF's tallest peak off by >3 BPM." % (bad, len(rows)))

    # how often is the COMB peak right where the ACF peak is not?
    acf_ok = comb_ok = both = neither = 0
    for k, ratio, b, m, med_ibi, lib in rows:
        x = _cache.load(k)
        odf = T.odf_cached(k, x, args.design, args.whiten)
        acf_bpm, _c, _g = T.estimate_acf(odf, win=T.WIN)
        comb_bpm, _s = T.estimate_comb(odf, T.WIN)
        if acf_bpm is None or comb_bpm is None:
            continue
        a_ok = abs(acf_bpm - lib) <= 3.0
        c_ok = abs(comb_bpm - lib) <= 3.0
        acf_ok += a_ok
        comb_ok += c_ok
        both += (a_ok and c_ok)
        neither += (not a_ok and not c_ok)
    n = acf_ok + comb_ok - both + neither
    print()
    print("against librosa's BPM, with a 3 BPM tolerance:")
    print("  acf right   %3d   comb right  %3d   both  %3d   neither %3d   (of %d)"
          % (acf_ok, comb_ok, both, neither, n))


if __name__ == "__main__":
    sys.exit(main())

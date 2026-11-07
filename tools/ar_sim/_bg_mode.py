#!/usr/bin/env python3
"""THE TWO MODES: DJ SET and PLAYLIST.

Troy's words:

  "We could also have a toggle button in the info panel that enables
   'DJ Set' mode versus 'Playlist' mode - 'DJ Set' meaning we expect smooth
   BPM and grid transitions, whereas 'Playlist' means anything could shuffle
   in"

What the modes mean for the TRACKER, and why they are not the same knob
twice:

  DJ SET     The music is CONTINUOUS.  One performance, one tempo that
             drifts, ramps and mixes.  A long memory is honest: the window
             holds one piece of music.  The grid that is working is
             trusted - the background has to CONVERGE and has to be clearly
             better before it may take the grid - and the user's downbeat
             survives a rate change, because a DJ's pitch change does not
             move the bar.

  PLAYLIST   The music is DISCONTINUOUS.  A window can straddle a track
             boundary, and a long memory then holds TWO tracks - which is
             how a twelve-second memory turns a two-second-old track into
             a mystery.  So the background's memory is SHORTER: it forgets
             the old track sooner, so the new one is found sooner.  The
             handover is quicker - fewer agreements needed, less margin
             over the steady rate - and the user's downbeat is DROPPED,
             because the new track's bar starts somewhere else.

The default is DJ SET: a DJ set is what the board is built for, and it is
the conservative one.

    python tools/ar_sim/_bg_mode.py
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _fw_bg as B                                     # noqa: E402
import _fw_port as P                                   # noqa: E402


# The two modes live in the twin, next to the constants they change.
MODES = B.MODES


def make(mode):
    t = B.BgTracker()
    t.set_mode(mode)
    return t


def run_odf(odf, mode):
    t = make(mode)
    t.run(odf)
    return t


def takeover(log, cut, want, tol=0.6, hold=40):
    """When did the rate settle on `want` after the cut at `cut`?"""
    cut_i = int(cut * P.RATE)
    on = np.abs(log - want) <= tol
    run = 0
    for i, v in enumerate(on[cut_i:], start=cut_i):
        run = run + 1 if v else 0
        if run >= hold:
            return (i - run + 1) / P.RATE - cut
    return None


def case_shuffle():
    """Five tracks, twenty seconds each, at five different tempi.

    This is the PLAYLIST case: anything could come next.
    """
    print("== the shuffle: five tracks, 20 s each")
    tracks = [(128.0, 0.0, 20.0, 1.0),
              (140.0, 20.0, 20.0, 1.0),
              (95.0, 40.0, 20.0, 1.0),
              (172.0, 60.0, 20.0, 1.0),
              (110.0, 80.0, 20.0, 1.0)]
    odf = B.beat_odf(tracks, dur=100.0)
    for mode in MODES:
        t = run_odf(odf, mode)
        log = np.array(t.bpm_log)
        outs = []
        for k in range(1, len(tracks)):
            want = tracks[k][0]
            cut = tracks[k][1]
            dt = takeover(log, cut, want)
            outs.append("%s->%s %s" % (tracks[k - 1][0], want,
                                        ("%.1f s" % dt) if dt else "never"))
        print("   %-9s  %s" % (mode, "   ".join(outs)))
        print("             final %7.2f (want 110)   change %d  refused %d"
              % (log[-1], t.changes, t.bg_refused))


def case_djset():
    """The DJ set: one performance, a ramp, and a big jump.

    The ramp must be followed.  The jump must be taken, and taken once.
    """
    print("== the DJ set: 128, a ramp to 134 over 20 s, then 145 at 60 s")
    ramp = lambda t: 128.0 + 6.0 * min(max((t - 20.0) / 20.0, 0.0), 1.0)
    odf = B.beat_odf([(ramp, 0.0, 60.0, 1.0),
                      (145.0, 60.0, 30.0, 1.0)], dur=90.0)
    for mode in MODES:
        t = run_odf(odf, mode)
        log = np.array(t.bpm_log)
        q = [B.med(log, s, s + 3) for s in (15, 30, 40, 55)]
        dt = takeover(log, 60.0, 145.0)
        print("   %-9s  at 15 s %7.2f  at 30 s %7.2f  at 40 s %7.2f  "
              "at 55 s %7.2f  (want 128/131/134/134)   the jump %s   "
              "change %d" % (mode, q[0], q[1], q[2], q[3],
                             ("%.1f s" % dt) if dt else "never", t.changes))


def case_regress():
    """The five cases must still pass in BOTH modes."""
    print("== the regression: the five cases, both modes")
    for mode in MODES:
        t = make(mode)
        # the breakdown
        odf = B.beat_odf([(128.0, 0.0, 20.0, 1.0),
                          (128.0, 25.0, 20.0, 1.0)], dur=45.0)
        log = np.array(run_odf(odf, mode).bpm_log)
        brk = float(np.max(log[int(21 * P.RATE):int(24 * P.RATE)])
                    - np.min(log[int(21 * P.RATE):int(24 * P.RATE)]))
        # the twitch
        odf = B.beat_odf([(128.0, 0.0, 15.0, 1.0),
                          (127.0, 15.0, 3.0, 1.0),
                          (128.0, 18.0, 15.0, 1.0)], dur=33.0)
        log = np.array(run_odf(odf, mode).bpm_log)
        tw = float(np.max(log[int(14 * P.RATE):int(19 * P.RATE)])
                   - np.min(log[int(14 * P.RATE):int(19 * P.RATE)]))
        # the breakout: a 160 on top of a 128
        odf = B.beat_odf([(128.0, 0.0, 30.0, 1.0),
                          (160.0, 0.0, 30.0, 1.0)], dur=30.0)
        log = np.array(run_odf(odf, mode).bpm_log)
        bo = B.med(log, 15)
        # the mixture: three seconds of 127 inside 128
        odf = B.beat_odf([(128.0, 0.0, 15.0, 1.0),
                          (127.0, 15.0, 3.0, 1.0),
                          (128.0, 18.0, 15.0, 1.0)], dur=33.0)
        print("   %-9s  breakdown %.2f   twitch %.2f   the 160 on top %7.2f"
              % (mode, brk, tw, bo))


def main():
    case_shuffle()
    case_djset()
    case_regress()
    return 0


if __name__ == "__main__":
    sys.exit(main())

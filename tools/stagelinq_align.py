"""
stagelinq_align.py - line StageLinq's two clocks up against each other.

The BeatInfo service gives an ABSOLUTE beat count (from the start of the
track) and the tempo.  The StateMap's /Engine/Deck{N}/BeatNumber gives the
deck's beat position INSIDE the bar (0..4, wrapping).  The one thing we
cannot get from either alone is WHERE THE DOWNBEAT IS: the absolute beat
number of the bar's first beat.

This script reads both services at once (one token, one main connection,
both service sockets open) and correlates them on the wall clock.  For every
BeatNumber sample it finds the BeatInfo beat at the same instant and prints
(beat - BeatNumber) mod 4.  A constant answers the question:

    the downbeat is at an absolute beat number congruent to that constant.

  python tools/stagelinq_align.py --ip 192.168.6.131 --seconds 12
"""

import argparse
import json
import struct
import sys
import time

sys.path.insert(0, "tools")
import stagelinq_probe as P


def interleaved(sm, bi, seconds):
    """Poll both sockets together and record (wall, value) for each."""
    sm.sock.settimeout(0.005)
    bi.sock.settimeout(0.005)
    end = time.monotonic() + seconds
    beats = []     # (wall, beat, samples)
    bns = []       # (wall, BeatNumber)
    names = {}     # path -> last value
    while time.monotonic() < end:
        sm.reader.poll()
        bi.reader.poll()
        # ---------------- StateMap
        r = sm.reader
        if r.avail() >= 4:
            ln = struct.unpack_from(">I", r.peek(4), 0)[0]
            if 8 <= ln <= 65536 and r.avail() >= 4 + ln:
                body, ts = r.try_take_timed(4 + ln)
                body = body[4:]
                if body[:4] == b"smaa" and struct.unpack_from(">I", body, 4)[0] == 0:
                    off = 8
                    n = struct.unpack_from(">I", body, off)[0]
                    off += 4
                    name = body[off:off + n].decode("utf-16-be", "replace")
                    off += n
                    n = struct.unpack_from(">I", body, off)[0]
                    off += 4
                    js = body[off:off + n].decode("utf-16-be", "replace")
                    try:
                        val = json.loads(js)
                    except Exception:
                        val = js
                    names[name] = val
                    if name.endswith("/BeatNumber"):
                        v = val.get("value") if isinstance(val, dict) else None
                        if v is not None:
                            bns.append((ts, v))
        # ---------------- BeatInfo
        r = bi.reader
        if r.avail() >= 4:
            x = struct.unpack_from(">I", r.peek(4), 0)[0]
            if x == 2:
                if r.avail() >= 16:
                    r.try_take(4)
                    r.try_take(8)                 # clock
                    decks = struct.unpack(">I", r.try_take(4))[0]
                    if r.avail() >= 24 * decks:
                        rows = []
                        for _ in range(decks):
                            rows.append(struct.unpack(">ddd", r.try_take(24)))
                        beats.append((ts, rows[0][0], rows[0][3] if len(rows[0]) > 3 else None))
            elif x > 8 and r.avail() >= 4 + x:
                body, ts = r.try_take_timed(4 + x)
                body = body[4:]
                if struct.unpack_from(">I", body, 0)[0] == 2 and len(body) >= 16:
                    decks = struct.unpack_from(">I", body, 12)[0]
                    off = 16
                    rows = []
                    for _ in range(decks):
                        if off + 24 > len(body):
                            break
                        rows.append(struct.unpack_from(">ddd", body, off))
                        off += 24
                    if rows:
                        smp = None
                        if len(body) - off >= 8 * len(rows):
                            smp = struct.unpack_from(">d", body, off)[0]
                        beats.append((ts, rows[0][0], smp))
        time.sleep(0.001)
    return beats, bns, names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default="192.168.6.131")
    ap.add_argument("--seconds", type=float, default=12.0)
    ap.add_argument("--discover-seconds", type=float, default=4.0)
    args = ap.parse_args()

    found = P.discover(args.discover_seconds, args.ip, False)
    cands = P.pick_devices(found, args.ip)
    if not cands:
        print("no device")
        return 1
    d = None
    for c in cands:
        if c["software"].lower().startswith("jp"):
            d = c
            break
    if d is None:
        d = cands[0]

    dev = P.Device(d["ip"], d["port"], P.os_urandom16())
    P.negotiate(dev, "random", True)
    if not dev.services:
        print("no service list")
        return 1

    # open BOTH service connections on this token, then keep both open
    bi = P.BeatInfo(dev)
    bi.connect()
    bi.start_stream()
    sm = P.StateMap(dev)
    sm.connect()
    for p in ["/Engine/Deck1/BeatNumber",
              "/Engine/Deck1/CurrentBPM",
              "/Engine/Deck1/Track/SongName",
              "/Engine/Deck1/Track/CuePosition",
              "/Engine/Deck1/Track/SampleRate",
              "/Engine/Deck1/DeckIsMaster"]:
        sm.subscribe(p)

    beats, bns, names = interleaved(sm, bi, args.seconds)
    print("\nbeat frames: %d    BeatNumber samples: %d" % (len(beats), len(bns)))
    for k, v in names.items():
        print("  %-36s %s" % (k, v))
    if len(beats) < 3 or len(bns) < 3:
        print("not enough data")
        return 1

    # beat as a function of wall time (the track is playing at a constant
    # rate, so a straight line fits it)
    t0 = beats[0][0]
    n = len(beats)
    sx = sum(b[0] - t0 for b in beats)
    sy = sum(b[1] for b in beats)
    sxx = sum((b[0] - t0) ** 2 for b in beats)
    sxy = sum((b[0] - t0) * b[1] for b in beats)
    den = n * sxx - sx * sx
    slope = (n * sxy - sx * sy) / den
    icept = (sy - slope * sx) / n
    print("\n  beat(t) = %.6f * t + %.6f   (%.4f beats/s)"
          % (slope, icept, slope))

    def beat_at(t):
        return slope * (t - t0) + icept

    # the alignment
    print("\n  wall       BeatNumber   beat(t)     (beat - BeatNumber) mod 4")
    offs = []
    for t, bn in bns[:40]:
        b = beat_at(t)
        d = (b - bn) % 4.0
        offs.append(d)
        print("  %.3f  %10.5f  %10.5f   %8.5f" % (t, bn, b, d))
    if offs:
        print("\n  offset: min %.5f  max %.5f  spread %.5f"
              % (min(offs), max(offs), max(offs) - min(offs)))
        print("  mean  %.5f" % (sum(offs) / len(offs)))

    # the WRAPS: BeatNumber going past 4 and back to 0 IS the downbeat.
    print("\n  the downbeats (BeatNumber wrapping past 4):")
    wraps = []
    for i in range(1, len(bns)):
        if bns[i][1] < bns[i - 1][1] - 1.0:
            t, bn = bns[i]
            b = beat_at(t)
            d = (b - bn) % 4.0
            wraps.append(d)
            print("    wall %.3f  BeatNumber %.5f  beat %.5f  -> downbeat at beat %8.4f  (mod 4 = %.4f)"
                  % (t, bn, b, b - bn, d))
    if wraps:
        med = sorted(wraps)[len(wraps) // 2]
        print("\n  DOWNBEAT at an absolute beat == %.4f (mod 4)   [median of %d wraps]"
              % (med, len(wraps)))
        print("  so  beat-in-bar = floor(beat - %.4f) mod 4 + 1" % med)
        print("  and the downbeat's absolute beat = %.4f + 4k" % med)
    else:
        print("  (no wrap seen - the capture was too short)")

    # the beat counter's ZERO: samples = a + b*beat
    pts = [(b[1], b[2]) for b in beats if b[2] is not None]
    if len(pts) > 5:
        n = len(pts)
        sx = sum(p[0] for p in pts); sy = sum(p[1] for p in pts)
        sxx = sum(p[0]*p[0] for p in pts); sxy = sum(p[0]*p[1] for p in pts)
        den = n*sxx - sx*sx
        slp = (n*sxy - sx*sy)/den
        icp = (sy - slp*sx)/n
        resid = max(abs(y - (icp + slp*x)) for x, y in pts)
        print("\n  samples = %.1f + %.4f * beat   (max residual %.1f samples = %.2f ms)"
              % (icp, slp, resid, resid/44100.0*1000))
        print("  -> %.2f samples per beat; the counter's zero is at sample %.1f (beat %.5f)"
              % (slp, icp, icp/slp))

    # the deck's own cue, for comparison
    cue = names.get("/Engine/Deck1/Track/CuePosition")
    bpm = names.get("/Engine/Deck1/CurrentBPM")
    if isinstance(cue, dict) and isinstance(bpm, dict):
        cs = cue.get("value")
        bpmv = bpm.get("value")
        if cs and bpmv:
            spb = 44100.0 * 60.0 / bpmv
            print("\n  the deck's own cue: %s samples -> %.5f beats at %.4f BPM"
                  % (cs, cs / spb, bpmv))

    bi.close()
    sm.close()
    dev.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

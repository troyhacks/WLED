"""
The second pass: measure the effects the autocorrelation could not call.

The autocorrelation is a blunt instrument - it finds a peak, but not
always the RIGHT peak.  This one measures the thing directly:

  - a FLASH or a PULSE: the interval between the signal's peaks.  The
    peaks are found with a minimum separation, so one flash cannot be
    counted twice.
  - a MOVER: the time between the head's crossings of the strip, taken
    from the brightness-weighted CENTRE of the lit pixels.  The centre
    is used rather than the brightest pixel because the brightest pixel
    flickers between the head and the tail.
  - a CHASE: the rate at which the head changes position, which is how
    many steps it takes per beat.

Usage:
    python tools/beat_compliance.py
"""
import asyncio
import json
import os
import time
import urllib.request

import websockets

HOST = os.environ.get("WLED_HOST", "192.168.6.249")
URL = f"ws://{HOST}/ws"
STATE = f"http://{HOST}/json/state"
AR = f"http://{HOST}/json/ar"

# fx, name, kind, expected ("beat" | "bar")
TESTS = [
    (233, "Beat Strobe",     "pulse", "beat"),
    (68,  "Bpm",             "pulse", "beat"),
    (1,   "Blink",           "pulse", "beat"),
    (26,  "Blink Rainbow",   "pulse", "beat"),
    (23,  "Strobe",          "pulse", "beat"),
    (13,  "Theater",         "chase", "beat"),
    (14,  "Theater Rainbow", "chase", "beat"),
    (37,  "Chase 2",         "chase", "beat"),
    (36,  "Sweep Random",    "pulse", "bar"),
    (239, "Tracker",         "mover", "bar"),
    (238, "Beat Wave",       "mover", "beat"),
]

SECS = 14.0


def get(url):
    with urllib.request.urlopen(url, timeout=8) as r:
        return json.loads(r.read().decode("utf-8"))


def post_state(body):
    req = urllib.request.Request(
        STATE, data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=8) as r:
        return json.loads(r.read().decode("utf-8"))


def decode(frame):
    if len(frame) < 4 or frame[0:1] != b"L":
        return None
    w, h = frame[2], frame[3]
    px = []
    for i in range(w * h):
        o = 4 + i * 3
        if o + 3 > len(frame):
            break
        px.append((frame[o], frame[o + 1], frame[o + 2]))
    return w, h, px


async def watch(seconds, out):
    async with websockets.connect(URL, max_size=None) as ws:
        await ws.send(json.dumps({"lv": True}))
        t0 = time.time()
        while time.time() - t0 < seconds:
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=seconds)
            except asyncio.TimeoutError:
                break
            if isinstance(raw, str):
                continue
            d = decode(raw)
            if not d:
                continue
            w, h, px = d
            out.append((time.time() - t0, w, h,
                        tuple((i % w, i // w, r + g + b)
                              for i, (r, g, b) in enumerate(px) if r + g + b > 24)))


def peaks(sig, times, min_sep=0.15, frac=0.25):
    """The times of the signal's peaks, one per pulse."""
    if not sig:
        return []
    hi = max(sig)
    lo = min(sig)
    if hi - lo < 1:
        return []
    thr = lo + (hi - lo) * frac
    out = []
    for i in range(1, len(sig) - 1):
        if sig[i] >= thr and sig[i] >= sig[i - 1] and sig[i] > sig[i + 1]:
            if out and times[i] - out[-1][0] < min_sep:
                if sig[i] > out[-1][1]:
                    out[-1] = (times[i], sig[i])
                continue
            out.append((times[i], sig[i]))
    return [t for t, _ in out]


def median(v):
    v = sorted(v)
    n = len(v)
    return v[n // 2] if n % 2 else (v[n // 2 - 1] + v[n // 2]) / 2.0


def main():
    before = get(STATE)["seg"][0]
    saved = {k: before[k] for k in ("fx", "sx", "ix", "c1", "c2", "c3",
                                     "o1", "o2", "o3", "col", "pal", "m12", "si")}
    tempo = get(AR)["AudioReactive"]["tempo"]
    bpm = tempo.get("bpm") or 0
    beat = 60.0 / bpm if bpm else 0.5
    print("the music: %.2f BPM   a beat %.0f ms   a bar %.0f ms\n"
          % (bpm, beat * 1000, beat * 4000))

    for fx, name, kind, per in TESTS:
        post_state({"seg": [{"id": 0, "fx": fx, "fxdef": True}]})
        time.sleep(0.8)
        frames = []
        asyncio.run(watch(SECS, frames))
        if not frames:
            print("%-16s NO FRAMES" % name)
            continue
        times = [t for t, _, _, _ in frames]
        sig = [sum(v for (_, _, v) in lit) for _, _, _, lit in frames]
        w = frames[0][1]
        want = beat * (4.0 if per == "bar" else 1.0)

        if kind == "pulse":
            pk = peaks(sig, times)
            gaps = [pk[i + 1] - pk[i] for i in range(len(pk) - 1)]
            meas = median(gaps) if gaps else None
            how = "%d peaks" % len(pk)
        elif kind == "chase":
            # The head: the brightest pixel on the first row, and how
            # often it moves.
            pos = []
            for (_, _, _, lit) in frames:
                row = [(x, v) for (x, y, v) in lit if y == 0]
                pos.append(max(row, key=lambda p: p[1])[0] if row else None)
            moves = 0
            last = None
            for p in pos:
                if p is None:
                    continue
                if last is not None and p != last:
                    moves += 1
                last = p
            span = times[-1] - times[0]
            meas = (span / moves) if moves else None
            how = "%d moves in %.1f s" % (moves, span)
        else:
            # The mover: the centre of the lit pixels, and its crossings.
            ctr = []
            for (_, _, _, lit) in frames:
                row = [(x, v) for (x, y, v) in lit if y == 0]
                tot = sum(v for _, v in row)
                ctr.append((sum(x * v for x, v in row) / tot) if tot else None)
            wraps = []
            for i in range(1, len(ctr)):
                if ctr[i] is None or ctr[i - 1] is None:
                    continue
                if ctr[i] < ctr[i - 1] - w * 0.4:
                    wraps.append(times[i])
            gaps = [wraps[i + 1] - wraps[i] for i in range(len(wraps) - 1)]
            meas = median(gaps) if gaps else None
            how = "%d crossings" % len(wraps)

        if meas:
            err = (meas - want) / want * 100.0
            ok = "OK " if abs(err) < 12 else "!! "
            print("%-16s %s %-16s %6.3f s  (want %6.3f s, %+5.1f%%)"
                  % (name, ok, how, meas, want, err))
        else:
            print("%-16s -- %-16s could not be measured" % (name, how))

    post_state({"seg": [dict(saved, id=0)]})
    print("\nrestored:", get(STATE)["seg"][0].get("fx"))


if __name__ == "__main__":
    main()

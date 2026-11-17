"""
Run every beat effect and check it against the beat.

For each effect: set it (with the effect's own defaults, the way the GUI
does), watch the liveview, and measure what it drew.

The measurement that matters is the PERIOD of the pattern, against the
beat the board's own tempo tracker says is running.  A flash that fires
on the beat and a mover that crosses once a bar both have to show it.

The period comes from two places and the harness has to be careful with
both:

  - the HEAD's wrap: for a mover, the time between one crossing of the
    strip and the next.  It is only trusted when the crossings are EVENLY
    spaced - an effect that pulses in place has a "brightest pixel" that
    jumps around, and those jumps are not crossings.
  - the AUTOCORRELATION of the brightness signal: for a flash.  The
    autocorrelation of any smooth signal is highest at lag 1, so the
    period is the first PEAK after the first MINIMUM, not the biggest
    peak.

The liveview is the websocket, not the HTTP API: it is fast and local,
and it does not count against the board's request budget.

Usage:
    python tools/beat_suite.py [seconds_per_effect]
"""
import asyncio
import json
import os
import sys
import time
import urllib.request

import websockets

HOST = os.environ.get("WLED_HOST", "192.168.6.249")
URL = f"ws://{HOST}/ws"
STATE = f"http://{HOST}/json/state"
AR = f"http://{HOST}/json/ar"

# Every effect that reads the beat, and what it is expected to do.
#   period: "beat" = one cycle per beat, "bar" = one per bar,
#           "phrase" = one per sixteen beats
EFFECTS = [
    (233, "Beat Strobe",     "beat",   "the reference: it already used the beat"),
    (68,  "Bpm",             "beat",   "the stripes pulse, peaking on the beat"),
    (1,   "Blink",           "beat",   "one on/off cycle per beat"),
    (26,  "Blink Rainbow",   "beat",   "one on/off cycle per beat"),
    (23,  "Strobe",          "beat",   "a flash on each beat"),
    (13,  "Theater",         "beat",   "the head advances one step per beat"),
    (14,  "Theater Rainbow", "beat",   "the head advances one step per beat"),
    (37,  "Chase 2",         "beat",   "the head advances one step per beat"),
    (100, "Heartbeat",       "bar",    "a lub-dub on the downbeat, once a bar"),
    (3,   "Wipe",            "bar",    "one sweep per bar, landing on the downbeat"),
    (6,   "Sweep",           "bar",    "one sweep per bar, reversed"),
    (4,   "Wipe Random",     "bar",    "one sweep per bar"),
    (36,  "Sweep Random",    "bar",    "one sweep per bar, reversed"),
    (235, "Beat Counter",    "bar",    "four markers; the one that is lit is the beat"),
    (237, "Bar Mover",       "bar",    "the light's position is the bar's phase"),
    (239, "Tracker",         "bar",    "the Bar Mover, plus the grid's quality"),
    (238, "Beat Wave",       "beat",   "the strip undulates on the beat's period"),
    (236, "Phrase",          "phrase", "sixteen steps, one lit"),
    (240, "Phrase Mover",    "phrase", "the light's position is the phrase's"),
]


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


def period_of(signal):
    """The period of a signal, from its autocorrelation.

    The autocorrelation of any SMOOTH signal is highest at lag 1, so the
    period is the first PEAK after the first MINIMUM - not the biggest
    peak, which is always the first one.
    """
    n = len(signal)
    if n < 12:
        return None
    mean = sum(signal) / n
    s = [v - mean for v in signal]
    ac = []
    for lag in range(n // 2):
        ac.append(sum(s[i] * s[i + lag] for i in range(n - lag)))
    start = 3
    for lag in range(3, len(ac) - 1):
        if ac[lag] < ac[lag - 1] and ac[lag] <= ac[lag + 1]:
            start = lag + 1
            break
    best, bestlag = 0.0, None
    for lag in range(start, len(ac)):
        if ac[lag] > best:
            best, bestlag = ac[lag], lag
    return bestlag


def wrap_period(frames, w):
    """The period of the head's crossings, if they are evenly spaced."""
    heads = []
    for (_, _, _, lit) in frames:
        row = [(x, v) for (x, y, v) in lit if y == 0]
        heads.append(max(row, key=lambda p: p[1])[0] if row else None)
    wraps = []
    for i in range(1, len(heads)):
        if heads[i] is None or heads[i - 1] is None:
            continue
        if heads[i] < heads[i - 1] - w * 0.5:
            wraps.append(frames[i][0] - frames[i - 1][0])
    if len(wraps) < 2:
        return None, 0
    mean = sum(wraps) / len(wraps)
    # A real crossing takes at least a quarter of a second; anything
    # faster is the brightest pixel flickering between neighbours.
    if mean <= 0.25:
        return None, len(wraps)
    # Evenly spaced?  A pattern that pulses in place has a "brightest
    # pixel" that jumps around, and those jumps are not crossings.
    spread = (max(wraps) - min(wraps)) / mean
    if spread > 0.35:
        return None, len(wraps)
    return mean, len(wraps)


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0

    before = get(STATE)["seg"][0]
    saved = {k: before[k] for k in ("fx", "sx", "ix", "c1", "c2", "c3",
                                     "o1", "o2", "o3", "col", "pal", "m12", "si")}
    print("saved:", {k: saved[k] for k in ("fx", "sx", "ix", "o1", "o2", "o3")})

    # The tempo, once: the music does not change while the suite runs.
    tempo = get(AR)["AudioReactive"]["tempo"]
    bpm = tempo.get("bpm") or 0
    beat_s = 60.0 / bpm if bpm else 0
    print("the music: %s BPM  (a beat is %.0f ms, a bar %.0f ms, a phrase %.0f ms)\n"
          % (bpm, beat_s * 1000, beat_s * 4000, beat_s * 16000))

    results = []
    for fx, name, per, what in EFFECTS:
        # fxdef: the effect's own defaults, the way the GUI applies them.
        post_state({"seg": [{"id": 0, "fx": fx, "fxdef": True}]})
        time.sleep(0.8)
        frames = []
        asyncio.run(watch(secs, frames))
        if not frames:
            print("%-16s  NO FRAMES" % name)
            results.append((name, "no frames"))
            continue

        w, h = frames[0][1], frames[0][2]
        sig = [sum(v for (_, _, v) in lit) for (_, _, _, lit) in frames]
        # The frame rate, from the timestamps.
        dt = (frames[-1][0] - frames[0][0]) / max(1, len(frames) - 1)

        per_meas, how = wrap_period(frames, w)
        if per_meas is None:
            lag = period_of(sig)
            how = "the signal"
            per_meas = (lag * dt) if lag else None

        want = {"beat": beat_s, "bar": beat_s * 4, "phrase": beat_s * 16}[per]
        if per_meas:
            err = (per_meas - want) / want * 100.0
            ok = "OK " if abs(err) < 15 else "!! "
        else:
            err = 0.0
            ok = "-- "
        print("%-16s %s %-13s %6.2f s  (want %6.2f s, %+5.1f%%)  lit %d..%d   %s"
              % (name, ok, how, per_meas or 0, want, err,
                 min(sig), max(sig), what))
        results.append((name, ok.strip(), per_meas, want, err))

    post_state({"seg": [dict(saved, id=0)]})
    time.sleep(0.5)
    back = get(STATE)["seg"][0]
    print("\nrestored:", {k: back.get(k) for k in ("fx", "sx", "ix", "o1", "o2", "o3")})

    bad = [r for r in results if len(r) > 1 and r[1] not in ("OK", "--")]
    print("%d effects, %d off their expected period" % (len(results), len(bad)))
    for b in bad:
        print("   ", b[0])


if __name__ == "__main__":
    main()

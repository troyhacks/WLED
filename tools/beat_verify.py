"""
Verify the beat-locked effects on the board.

Sets a segment to an effect, watches the liveview websocket WHILE polling
the tempo, and compares the two.

The wall's marker changes and the board's beat are both derived from the
SAME published tempo frame, so they must agree.  The measurement cannot
prove it directly, because the board's JSON is slow - a request takes about
a quarter of a second, which is half a beat - so the time a sample is
stamped with is uncertain by that much.

What CAN be measured exactly is the OFFSET between the wall's change and
the beat boundary the sample implies, and whether that offset DRIFTS.  A
constant offset is the HTTP path's serving-time bias; a growing one would
mean the wall and the board had come apart.

Usage:
    python tools/beat_verify.py [effect_id] [seconds]

    effect_id  235 = Beat Counter, 236 = Phrase (default 235)
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

# The Beat Counter's four markers, as the clock face reads them.
BEAT_POS = {(2, 0): 1, (3, 2): 2, (2, 3): 3, (0, 2): 4}


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
            lit = [(i % w, i // w, r + g + b)
                   for i, (r, g, b) in enumerate(px) if r + g + b > 24]
            out.append((time.time() - t0, lit))


async def poll_tempo(seconds, out):
    """(time, beat, progress, bpm) for the whole window."""
    t0 = time.time()
    while time.time() - t0 < seconds:
        t1 = time.time()
        try:
            t = get(AR)["AudioReactive"]["tempo"]
            out.append(((t1 + time.time()) * 0.5 - t0, t.get("beat_in_bar"),
                        t.get("beat_progress"), t.get("bpm")))
        except Exception as e:
            out.append(((t1 + time.time()) * 0.5 - t0, None, None, str(e)))
        await asyncio.sleep(0.05)


def main():
    fx = int(sys.argv[1]) if len(sys.argv) > 1 else 235
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 14.0

    before = get(STATE)["seg"][0]
    saved = {k: before[k] for k in ("fx", "sx", "ix", "c1", "c2", "c3",
                                     "o1", "o2", "o3", "col", "pal", "m12", "si")}
    print("saved:", {k: saved[k] for k in ("fx", "sx", "ix", "o1", "o2", "o3")})

    # The third colour slot is the downbeat's, and the second is the off
    # colour.  Blue for the beat, black for off, white for the downbeat.
    post_state({"seg": [{"id": 0, "fx": fx, "sx": 128, "ix": 96,
                         "col": [[0, 0, 255, 0], [0, 0, 0, 0], [255, 255, 255, 0]]}]})
    time.sleep(0.5)

    frames, tempo = [], []

    async def both():
        await asyncio.gather(watch(seconds, frames), poll_tempo(seconds, tempo))

    asyncio.run(both())

    # The wall's changes.
    changes = []
    prev = None
    for t, lit in frames:
        key = tuple((x, y) for (x, y, v) in lit)
        if key != prev:
            changes.append((t, key))
            prev = key

    # The boundary each tempo sample implies.
    implied = []
    for (ts, beat, prog, bpm) in tempo:
        if beat is None or prog is None or not bpm:
            continue
        implied.append((ts - prog * (60.0 / bpm), beat))

    print("\n--- the wall's changes against the board's beat ---")
    offs = []
    seq = []
    for t, key in changes:
        if len(key) != 1:
            continue
        beat = BEAT_POS.get(key[0])
        if beat is None:
            continue
        seq.append((t, beat))
        cands = [b for (b, bb) in implied if bb == beat]
        if not cands:
            continue
        b = min(cands, key=lambda x: abs(x - t))
        if abs(b - t) > 0.6:
            continue
        offs.append((t, (t - b) * 1000.0))
        print(f"  {t:7.3f}s  beat {beat}  lit {key[0]}  "
              f"offset {((t - b) * 1000.0):+7.0f} ms")

    bpm = None
    for (ts, beat, prog, b) in tempo:
        if b:
            bpm = b
            break
    period = 60.0 / bpm if bpm else 0.469

    print(f"\n  the board's tempo: {bpm} BPM, so a beat is {period*1000:.0f} ms")

    if len(seq) > 3:
        # The wall's own rate: a straight line through its change times.
        # The slope is the period it is actually running at, and the
        # residuals say how steady it is.  A wall locked to the music has a
        # slope equal to the tracked beat, and residuals that do not grow.
        n = len(seq)
        mt = sum(t for t, _ in seq) / n
        mi = sum(range(n)) / n
        num = sum((i - mi) * (t - mt) for i, (t, _) in enumerate(seq))
        den = sum((i - mi) ** 2 for i in range(n))
        slope = num / den if den else 0.0
        t0 = mt - slope * mi
        res = [t - (t0 + slope * i) for i, (t, _) in enumerate(seq)]
        rms = (sum(r * r for r in res) / n) ** 0.5
        # The drift: the residuals' end-to-end change, which is what says
        # whether the wall is falling behind the music.
        drift = (res[-1] - res[0]) * 1000.0

        # The order the beats came in.
        order = "".join(str(b) for _, b in seq)
        expected = "".join(str((seq[0][1] - 1 + i) % 4 + 1) for i in range(n))

        print(f"  the wall's period: {slope*1000:.1f} ms  "
              f"(the music's is {period*1000:.1f} ms, "
              f"{(slope-period)*1000:+.1f} ms)")
        print(f"  residual RMS {rms*1000:.0f} ms, drift {drift:+.0f} ms over "
              f"{seq[-1][0]-seq[0][0]:.1f} s")
        print(f"  the order the beats came in: {order}")
        print(f"  what it should have been:        {expected}")
        print("  " + ("THE ORDER IS RIGHT" if order == expected
                      else "THE ORDER IS WRONG"))

    if offs:
        vals = [o for _, o in offs]
        print(f"\n  the offset from the board's own boundary: mean "
              f"{sum(vals)/len(vals):+.0f} ms, spread "
              f"{max(vals)-min(vals):.0f} ms")
        print("  The spread is the JSON request's own time - the board serves")
        print("  it somewhere inside those 250 ms - so this is the PATH's")
        print("  uncertainty, not the effect's.")

    post_state({"seg": [dict(saved, id=0)]})
    print("restored:", {k: saved[k] for k in ("fx", "sx", "ix", "o1", "o2", "o3")})


if __name__ == "__main__":
    main()

"""
Does the Beat Sweep's CYCLE advance when it should?

The Cycle slider is the number of BARS each sub-effect is shown for; its
default is 4 bars, which is 16 beats - the same sixteen as the phrase, so
the cycle's slot and the phrase are the same grid.

The slot is the master's bar number divided by the bars-per-slot, so the
slot turns over when the master's count turns over a multiple of sixteen.
The endpoint publishes the deck's count as source_32 (its position in the
song in beats, modulo 32), and the count turns 32 -> 1 at exactly the
sixteen-beat boundary.

So: the sub-effect should change exactly when source_32 wraps.

Usage:
    python tools/beat_cycle.py [seconds]
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


def get(url):
    with urllib.request.urlopen(url, timeout=10) as r:
        return json.loads(r.read().decode("utf-8"))


def post_state(body):
    req = urllib.request.Request(
        STATE, data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=12) as r:
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


def signature(w, px):
    """A shape that changes when the sub-effect does."""
    n = len(px) // 3
    lit = [(i % w, px[i * 3] + px[i * 3 + 1] + px[i * 3 + 2]) for i in range(n)]
    lit = [(x, v) for (x, v) in lit if v[1] > 24]
    if not lit:
        return None
    xs = [x for (x, v) in lit]
    return (len(lit), max(xs) - min(xs))


async def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0

    before = get(STATE)["seg"][0]
    saved = {k: before.get(k) for k in ("fx", "sx", "ix", "c1", "c2", "c3",
                                        "o1", "o2", "o3", "col", "pal", "m12", "si")}
    # The Beat Sweep with the cycle at its default: sx = 128 is 4 bars.
    post_state({"seg": [{"id": 0, "fx": 241, "sx": 128, "ix": 128, "c1": 96, "c3": 12,
                         "o1": True, "o2": False, "o3": False, "pal": 11,
                         "col": [[0, 0, 255, 0], [0, 0, 0, 0], [255, 255, 0, 0]]}]})
    time.sleep(1.0)

    frames = []
    counts = []

    async def poll():
        t0 = time.time()
        while time.time() - t0 < seconds:
            try:
                t = get(AR)["AudioReactive"]["tempo"]
                counts.append((time.time() - t0, t.get("source_32"), t.get("source_beat")))
            except Exception:
                pass
            await asyncio.sleep(0.25)

    async def watch():
        t0 = time.time()
        async with websockets.connect(URL, max_size=None) as ws:
            await ws.send(json.dumps({"lv": True}))
            while time.time() - t0 < seconds:
                try:
                    raw = await asyncio.wait_for(ws.recv(), timeout=5)
                except asyncio.TimeoutError:
                    continue
                except Exception:
                    break
                if isinstance(raw, str):
                    continue
                d = decode(raw)
                if not d:
                    continue
                w, h, px = d
                frames.append((time.time() - t0, w, signature(w, px)))

    try:
        await asyncio.gather(watch(), poll())
    finally:
        try:
            post_state({"seg": [{"id": 0, **saved}]})
        except Exception as e:
            print("restore failed:", e)

    # The count's turns: 32 -> 1 is a sixteen-beat boundary.
    turns = []
    for i in range(1, len(counts)):
        if counts[i][1] == 1 and counts[i - 1][1] == 32:
            turns.append(counts[i][0])
    print(f"\n  the count turned over sixteen-beat {len(turns)} times: "
          f"{[round(t,1) for t in turns]}")

    # The sub-effect's changes: the shape changed by a lot.
    changes = []
    prev = None
    for t, w, sig in frames:
        if sig is None:
            continue
        if prev is not None and abs(sig[0] - prev[0]) > max(6, prev[0] // 4):
            changes.append((t, prev, sig))
        prev = sig
    print(f"  the shape changed {len(changes)} times:")
    for t, a, b in changes:
        near = [round(x - t, 2) for x in turns if abs(x - t) < 1.5]
        print(f"    {t:7.3f}s  {a} -> {b}   {'at a sixteen-beat turn: ' + str(near) if near else 'NOT at one'}")


if __name__ == "__main__":
    asyncio.run(main())

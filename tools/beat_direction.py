"""
Does the sweep turn round ON the bar line?

The Beat Sweep's sweep goes one way for a bar and the other way for the
next, so the direction flips at the bar line.  If it flips somewhere else,
the light reverses in the middle of a bar.

The light's direction comes from the liveview (the light's position from
frame to frame), and the bar line from the tempo stream (the beat going
from four back to one).

The cycle is OFF here (the speed slider at its lowest) so the sub-effect
is the fixed one and the light's path is a clean sweep - the direction
does not depend on the cycle.

Usage:
    python tools/beat_direction.py [seconds]
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


async def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 40.0

    before = get(STATE)["seg"][0]
    saved = {k: before.get(k) for k in ("fx", "sx", "ix", "c1", "c2", "c3",
                                        "o1", "o2", "o3", "col", "pal", "m12", "si")}
    # The cycle off (speed 0), the inverse sub-effect (custom3 0): the plain
    # background with only the light on it, so the light's position is the
    # sweep and nothing else.
    post_state({"seg": [{"id": 0, "fx": 241, "sx": 0, "ix": 128, "c1": 96, "c3": 0,
                         "o1": True, "o2": False, "o3": False, "pal": 0,
                         "col": [[0, 0, 255, 0], [0, 0, 0, 0], [255, 255, 0, 0]]}]})
    time.sleep(1.0)

    grid = []
    light = []

    async def tempo_reader(stop):
        async with websockets.connect(URL, max_size=None) as ws:
            await ws.send(json.dumps({"tempo": True}))
            while not stop.is_set():
                try:
                    raw = await asyncio.wait_for(ws.recv(), timeout=2)
                except asyncio.TimeoutError:
                    continue
                except Exception:
                    break
                if not isinstance(raw, str):
                    continue
                try:
                    t = json.loads(raw).get("tempo")
                except Exception:
                    continue
                if t:
                    grid.append((time.time(), t.get("beat"), t.get("prog")))

    stop = asyncio.Event()
    reader = asyncio.create_task(tempo_reader(stop))

    async def live():
        t0 = time.time()
        async with websockets.connect(URL, max_size=None) as ws:
            await ws.send(json.dumps({"lv": True}))
            while time.time() - t0 < seconds:
                try:
                    raw = await asyncio.wait_for(ws.recv(), timeout=3)
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
                best, bx = -1, None
                for i, (r, g, b) in enumerate(px):
                    v = r + g + b
                    if v > best:
                        best, bx = v, i % w
                if bx is not None and best > 100:
                    light.append((time.time(), bx))

    try:
        await live()
    finally:
        stop.set()
        try:
            post_state({"seg": [{"id": 0, **saved}]})
        except Exception as e:
            print("restore failed:", e)

    # The bar lines: the beat going from four back to one.
    lines = []
    for i in range(1, len(grid)):
        if grid[i][1] == 1 and grid[i - 1][1] == 4:
            lines.append(grid[i][0])

    # The direction flips: the light's x changing sign.
    flips = []
    for i in range(2, len(light)):
        d1 = light[i - 1][1] - light[i - 2][1]
        d2 = light[i][1] - light[i - 1][1]
        if d1 * d2 < 0 and abs(d1) >= 1 and abs(d2) >= 1:
            flips.append(light[i][0])

    print(f"\n  {len(lines)} bar lines and {len(flips)} direction flips over {seconds:.0f} s")
    print("\n  the flip against the bar line it belongs to:")
    for f in flips:
        near = [round(l - f, 3) for l in lines if abs(l - f) < 1.2]
        if near:
            print(f"    flip at {f:.3f}s   bar line {near[0]:+.3f}s")
        else:
            print(f"    flip at {f:.3f}s   NO BAR LINE NEAR IT")


if __name__ == "__main__":
    asyncio.run(main())

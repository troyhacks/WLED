"""
Watch the Beat Sweep on the wall: the light's path, and when the sub-effect
changes.

The light's position is the bar's phase, so its path says whether the grid
is running straight.  The sub-effect changes when the CYCLE's slot moves,
which is the bar number divided by the bars-per-slot - so the changes say
whether the BARS are being counted right.

Usage:
    python tools/beat_sweep_watch.py [seconds] [loop]
        loop: 0 = bounce (default), 1 = loop
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
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 14.0
    loop_mode = int(sys.argv[2]) if len(sys.argv) > 2 else 0

    before = get(STATE)["seg"][0]
    saved = {k: before.get(k) for k in ("fx", "sx", "ix", "c1", "c2", "c3",
                                        "o1", "o2", "o3", "col", "pal", "m12", "si")}
    print("saved:", {k: saved[k] for k in ("fx", "sx", "ix", "c1", "c2", "c3", "o1", "o2", "o3")})

    # The Beat Sweep with the cycle on (slider 0 = 2 bars a sub-effect) and
    # the loop as asked for.
    post_state({"seg": [{"id": 0, "fx": 241, "sx": 128, "ix": 128, "c1": 96, "c3": 12,
                         "o1": True, "o2": bool(loop_mode), "o3": False,
                         "col": [[0, 0, 255, 0], [0, 0, 0, 0], [255, 255, 0, 0]]}]})
    time.sleep(1.0)

    rows = []
    try:
        async with websockets.connect(URL, max_size=None) as ws:
            await ws.send(json.dumps({"lv": True}))
            await ws.send(json.dumps({"tempo": True}))
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
                lit = [(i % w, i // w, r + g + b) for i, (r, g, b) in enumerate(px) if r + g + b > 24]
                rows.append((time.time() - t0, w, lit))
    finally:
        try:
            post_state({"seg": [{"id": 0, **saved}]})
        except Exception as e:
            print("restore failed:", e)

    print(f"\n--- {len(rows)} frames over {seconds:.0f} s ---")
    # The light's centre of mass along the strip, and the pattern's shape.
    prev_shape = None
    prev_pos = None
    for t, w, lit in rows:
        if not lit:
            continue
        xs = [x for (x, y, v) in lit]
        pos = sum(xs) / len(xs)
        shape = (len(lit), max(xs) - min(xs), max(v for (_, _, v) in lit))
        if shape != prev_shape:
            print(f"  {t:7.3f}s  PATTERN changed: {shape}  (light at x {pos:.1f})")
            prev_shape = shape
        elif prev_pos is None or abs(pos - prev_pos) > 1.5:
            print(f"  {t:7.3f}s  light at x {pos:.1f}")
        prev_pos = pos


if __name__ == "__main__":
    asyncio.run(main())

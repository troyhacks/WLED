"""
The Beat Sweep's path: where the light turns round, and when.

The light's position is the bar's phase, so the TURNING POINTS are the
bar boundaries.  Their spacing should be the bar's length (60000/bpm ms)
and the direction should alternate: left, right, left, right.

Usage:
    python tools/sweep_check.py [seconds]
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
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0

    before = get(STATE)["seg"][0]
    saved = {k: before.get(k) for k in ("fx", "sx", "ix", "c1", "c2", "c3",
                                        "o1", "o2", "o3", "col", "pal", "m12", "si")}
    post_state({"seg": [{"id": 0, "fx": 241, "sx": 128, "ix": 128, "c1": 96, "c3": 12,
                         "o1": True, "o2": False, "o3": False,
                         "col": [[0, 0, 255, 0], [0, 0, 0, 0], [255, 255, 0, 0]]}]})
    time.sleep(1.0)

    rows = []
    try:
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
                lit = [(i % w, i // w, r + g + b) for i, (r, g, b) in enumerate(px)
                       if r + g + b > 24]
                if lit:
                    xs = [x for (x, y, v) in lit]
                    rows.append((time.time() - t0, sum(xs) / len(xs),
                                 max(xs) - min(xs), len(lit)))
    finally:
        try:
            post_state({"seg": [{"id": 0, **saved}]})
        except Exception as e:
            print("restore failed:", e)

    print(f"--- {len(rows)} frames over {seconds:.0f} s ---")
    # The turning points: where the direction reverses.
    turns = []
    for i in range(2, len(rows) - 2):
        a, b, c = rows[i - 2][1], rows[i][1], rows[i + 2][1]
        if (a - b) * (c - b) < 0 and abs(a - c) < 12:
            side = "LEFT " if b < 20 else "RIGHT" if b > 36 else "MID  "
            if not turns or abs(turns[-1][0] - rows[i][0]) > 0.35:
                turns.append((rows[i][0], side, b))
    print("  turning points:")
    prev = None
    for t, side, x in turns:
        gap = "" if prev is None else "   (+%.3f s)" % (t - prev)
        print("    %7.3f s  %s  x %5.1f%s" % (t, side, x, gap))
        prev = t


if __name__ == "__main__":
    asyncio.run(main())

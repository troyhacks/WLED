"""
Watch one effect on the board and report how its pattern repeats.

Sets a segment to an effect, watches the liveview websocket, and prints
every change of the lit pixels with the time - so a Wipe that now runs once
per BAR, or a Chase that steps once per BEAT, can be read straight off.

Usage:
    python tools/beat_watch.py [effect_id] [seconds]
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
            out.append((time.time() - t0,
                        tuple((i % w, i // w, r + g + b)
                              for i, (r, g, b) in enumerate(px) if r + g + b > 24)))


async def poll(seconds, out):
    t0 = time.time()
    while time.time() - t0 < seconds:
        t1 = time.time()
        try:
            t = get(AR)["AudioReactive"]["tempo"]
            out.append(((t1 + time.time()) * 0.5 - t0, t.get("beat_in_bar"),
                        t.get("bpm")))
        except Exception:
            pass
        await asyncio.sleep(0.1)


def main():
    fx = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0

    before = get(STATE)["seg"][0]
    saved = {k: before[k] for k in ("fx", "sx", "ix", "c1", "c2", "c3",
                                     "o1", "o2", "o3", "col", "pal", "m12", "si")}
    print("saved:", {k: saved[k] for k in ("fx", "sx", "ix", "o1", "o2", "o3")})
    post_state({"seg": [{"id": 0, "fx": fx, "sx": 255, "ix": 128,
                         "col": [[0, 0, 255, 0], [0, 0, 0, 0], [255, 255, 255, 0]]}]})
    time.sleep(0.5)

    frames, tempo = [], []

    async def both():
        await asyncio.gather(watch(seconds, frames), poll(seconds, tempo))

    asyncio.run(both())

    print("\n--- the wall ---")
    prev = None
    for t, lit in frames:
        key = tuple((x, y) for (x, y, v) in lit)
        if key != prev:
            print(f"  {t:7.3f}s  {key}")
            prev = key

    bpm = None
    for (_, _, b) in tempo:
        if b:
            bpm = b
            break
    if bpm:
        print(f"\n  the music: {bpm:.2f} BPM -> a beat is {60000/bpm:.0f} ms, "
              f"a bar is {240000/bpm:.0f} ms")

    post_state({"seg": [dict(saved, id=0)]})
    print("restored:", {k: saved[k] for k in ("fx", "sx", "ix", "o1", "o2", "o3")})


if __name__ == "__main__":
    main()

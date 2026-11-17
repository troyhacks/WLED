"""
Draw what the beat effects actually look like on the strip.

For each effect: set it, watch the liveview, and print the first row as
ASCII - the same picture Troy is looking at on the bar, so it can be
checked here without eyes on the rig.

Usage:
    python tools/beat_filmstrip.py
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

RAMP = " .:-=+*#%@"

TESTS = [
    (235, "Beat Counter"),
    (236, "Phrase"),
    (237, "Bar Mover"),
    (238, "Beat Wave"),
    (239, "Tracker"),
    (240, "Phrase Mover"),
    (68,  "Bpm"),
    (100, "Heartbeat"),
    (3,   "Wipe"),
    (13,  "Theater"),
    (1,   "Blink"),
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
            out.append((time.time() - t0, w, h, px))


def ascii_row(px, w, h):
    """The first row as characters, brightest last."""
    out = []
    for x in range(w):
        r, g, b = px[x]
        v = r + g + b
        k = min(len(RAMP) - 1, v * (len(RAMP) - 1) // 765)
        out.append(RAMP[k])
    return "".join(out)


def main():
    before = get(STATE)["seg"][0]
    saved = {k: before[k] for k in ("fx", "sx", "ix", "c1", "c2", "c3",
                                     "o1", "o2", "o3", "col", "pal", "m12", "si")}
    tempo = get(AR)["AudioReactive"]["tempo"]
    bpm = tempo.get("bpm") or 0
    print("the music: %.2f BPM  (a beat %.0f ms)\n" % (bpm, 60000 / bpm if bpm else 0))

    for fx, name in TESTS:
        post_state({"seg": [{"id": 0, "fx": fx, "fxdef": True}]})
        time.sleep(0.8)
        frames = []
        asyncio.run(watch(3.2, frames))
        if not frames:
            print("%s: no frames" % name)
            continue
        w, h = frames[0][1], frames[0][2]
        print("=== %-14s  %d wide x %d tall ===" % (name, w, h))
        # A few moments spread across the window.
        picks = [frames[int(i * (len(frames) - 1) / 5)] for i in range(6)]
        for (t, _, _, px) in picks:
            print("   %5.2fs |%s|" % (t, ascii_row(px, w, h)))
        print()

    post_state({"seg": [dict(saved, id=0)]})
    print("restored:", get(STATE)["seg"][0].get("fx"))


if __name__ == "__main__":
    main()

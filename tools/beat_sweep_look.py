"""
Look at the Beat Sweep on the bar.

Sets the effect with the sub-effect slider at a few values, prints the
strip as ASCII at each, and measures the sweep: how long one pass takes
and which way it is going.

The board's state is restored at the end.
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

# fx 241 = Beat Sweep.  custom3 (c3) picks the sub-effect: 0..31.
SUBS = [
    (1,  "the ribbon, fixed colour"),
    (5,  "the ribbon, the band under the light"),
    (9,  "the ribbon, the dominant frequency"),
    (13, "the ribbon, the volume"),
    (17, "the ribbon, the band's value"),
    (21, "the ribbon, the light's position"),
    (25, "the ribbon, the dominant band"),
    (4,  "no ribbon, the band under the light"),
    (2,  "the mirrored ribbon"),
    (3,  "the steps"),
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


def ascii_row(px, w):
    out = []
    for x in range(w):
        r, g, b = px[x]
        v = (r + g + b) / 765.0
        k = min(len(RAMP) - 1, int((v ** 0.5) * (len(RAMP) - 1) + 0.5))
        out.append(RAMP[k])
    return "".join(out)


def main():
    before = get(STATE)["seg"][0]
    saved = {k: before[k] for k in ("fx", "sx", "ix", "c1", "c2", "c3",
                                     "o1", "o2", "o3", "col", "pal", "m12", "si")}
    tempo = get(AR)["AudioReactive"]["tempo"]
    bpm = tempo.get("bpm") or 0
    print("the music: %.2f BPM   a beat %.0f ms   a bar %.0f ms\n"
          % (bpm, 60000 / bpm if bpm else 0, 240000 / bpm if bpm else 0))

    for sub, what in SUBS:
        post_state({"seg": [{"id": 0, "fx": 241, "sx": 255, "ix": 128,
                             "c1": 96, "c2": 64, "c3": sub,
                             "o1": True, "o2": False, "o3": False}]})
        time.sleep(1.0)
        frames = []
        asyncio.run(watch(3.4, frames))
        if not frames:
            print("%-34s no frames" % what)
            continue
        w, h = frames[0][1], frames[0][2]
        print("=== sub %2d: %-34s ===" % (sub, what))
        picks = [frames[int(i * (len(frames) - 1) / 4)] for i in range(5)]
        for (t, _, _, px) in picks:
            print("   %5.2fs |%s|" % (t, ascii_row(px, w)))
        print()

    # The sweep itself: where the light is, and which way it is going.
    post_state({"seg": [{"id": 0, "fx": 241, "sx": 255, "ix": 128,
                         "c1": 96, "c2": 64, "c3": 1,
                         "o1": True, "o2": False, "o3": False}]})
    time.sleep(1.0)
    frames = []
    asyncio.run(watch(9.0, frames))
    print("=== the sweep: the brightest pixel over 9 s ===")
    prev = None
    for (t, w, h, px) in frames:
        row = [(x, px[x][0] + px[x][1] + px[x][2]) for x in range(w)]
        if not any(v for _, v in row):
            continue
        hx = max(row, key=lambda p: p[1])[0]
        if hx != prev:
            d = "" if prev is None else ("  <-" if hx < prev else "  ->")
            print("   %6.2fs  x=%2d%s" % (t, hx, d))
            prev = hx

    post_state({"seg": [dict(saved, id=0)]})
    print("\nrestored:", get(STATE)["seg"][0].get("fx"))


if __name__ == "__main__":
    main()

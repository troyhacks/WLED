"""
The wall and the screen, on one socket: do they agree on the beat?

The liveview frames and the tempo frames arrive on the SAME websocket, so
they can be compared frame by frame with no timing guesswork:

  - the liveview carries the wall (the Beat Counter's marker),
  - the tempo frame carries what the GUI's panel shows.

If the wall's beat and the panel's beat disagree, that is the "the wall
and the screen cannot disagree" rule broken - and if they agree but the
BEAT COUNT is wrong, the count is the thing to look at.

Usage:
    python tools/beat_agree.py [seconds]
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


async def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0

    before = get(STATE)["seg"][0]
    saved = {k: before.get(k) for k in ("fx", "sx", "ix", "c1", "c2", "c3",
                                        "o1", "o2", "o3", "col", "pal", "m12", "si")}
    # The Beat Counter in its strip layout: the marker at x = (b-1)*w/4 + w/8.
    post_state({"seg": [{"id": 0, "fx": 235, "sx": 128, "ix": 96, "o1": True, "o2": True,
                         "col": [[0, 0, 255, 0], [0, 0, 0, 0], [255, 255, 0, 0]]}]})
    time.sleep(0.8)

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
                    try:
                        j = json.loads(raw)
                    except Exception:
                        continue
                    t = j.get("tempo")
                    if t:
                        rows.append((time.time() - t0, "tempo", t.get("beat"), t.get("prog")))
                    continue
                d = decode(raw)
                if not d:
                    continue
                w, h, px = d
                lit = [i % w for i, (r, g, b) in enumerate(px) if r + g + b > 24]
                if not lit:
                    continue
                beat = round((min(lit) - w / 8) * 4 / w) + 1
                rows.append((time.time() - t0, "wall", beat, None))
    finally:
        post_state({"seg": [{"id": 0, **saved}]})

    print("\n--- the wall's beat and the panel's beat, in time order ---")
    for t, kind, beat, prog in rows:
        if kind == "wall":
            print(f"  {t:7.3f}s  wall  beat {beat}")
        else:
            print(f"  {t:7.3f}s  panel beat {beat}  prog {prog}")

    t = get(AR)["AudioReactive"]["tempo"]
    print(f"\n  the board: source {t.get('source')}, source_beat {t.get('source_beat')}, "
          f"source_32 {t.get('source_32')}, our beat {t.get('beat_in_bar')}, "
          f"our count {t.get('beat_count')}, our 32 {t.get('beat_in_measure32')}")


if __name__ == "__main__":
    asyncio.run(main())

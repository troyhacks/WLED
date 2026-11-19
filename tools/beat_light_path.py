"""
The light's path against the grid it should be on.

The Beat Sweep in Loop mode puts the light at the BAR's phase, so the light
sweeps left to right and wraps at the bar line.  The tempo stream says
where the grid is (the beat in the bar and the progress through it), so
the light's position and the grid's position can be compared directly.

The difference between them is the publish layer's HANDOFF correction: it
is what stops the light from teleporting when the grid moves.  It should
be zero almost all the time, and it should NOT swing when the deck's
grid moves for a good reason (a new song: the deck's position in the
song restarts, and with it the beat in the bar).

TWO sockets: the tempo stream is 25 Hz and keeps the socket's queue full,
so the live frames never get sent on the same one.

Usage:
    python tools/beat_light_path.py [seconds]
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


async def tempo_reader(stop, grid):
    """The grid: the last tempo frame, with the time it arrived."""
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
                j = json.loads(raw)
            except Exception:
                continue
            t = j.get("tempo")
            if t:
                grid.append((time.time(), t.get("beat"), t.get("prog"), t.get("plink")))


async def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0

    before = get(STATE)["seg"][0]
    saved = {k: before.get(k) for k in ("fx", "sx", "ix", "c1", "c2", "c3",
                                        "o1", "o2", "o3", "col", "pal", "m12", "si")}
    # The Beat Sweep, Loop on (o2), so the light's place is the bar's phase.
    # The cycle is off (sx = 0) and the sub-effect is the INVERSE layer
    # (c3 = 0): the plain background with only the light on it, so the
    # brightest pixel IS the light and not a band of the spectrum.
    post_state({"seg": [{"id": 0, "fx": 241, "sx": 0, "ix": 128, "c1": 96, "c3": 0,
                         "o1": True, "o2": True, "o3": False, "pal": 0,
                         "col": [[0, 0, 255, 0], [0, 0, 0, 0], [255, 255, 0, 0]]}]})
    time.sleep(1.0)

    grid = []
    stop = asyncio.Event()
    reader = asyncio.create_task(tempo_reader(stop, grid))
    rows = []
    t0 = time.time()
    try:
        # The liveview socket goes quiet, and the board drops it now and then
        # - so it is reconnected and the gap is bridged.
        while time.time() - t0 < seconds:
            try:
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
                        if not d or not grid:
                            continue
                        w, h, px = d
                        best, bx = -1, None
                        for i, (r, g, b) in enumerate(px):
                            v = r + g + b
                            if v > best:
                                best, bx = v, i % w
                        if bx is None or best < 100:
                            continue
                        gt, gbeat, gprog, gplink = grid[-1]
                        if gbeat is None:
                            continue
                        want = ((gbeat - 1) + (gprog or 0.0)) / 4.0 * w
                        rows.append((time.time() - t0, bx, want, gbeat, gprog, gplink, len(grid)))
            except Exception as e:
                print("  (the live socket dropped:", e, "- reconnecting)")
                await asyncio.sleep(1)
    finally:
        stop.set()
        try:
            post_state({"seg": [{"id": 0, **saved}]})
        except Exception as e:
            print("restore failed:", e)

    print(f"\n--- {len(rows)} samples over {seconds:.0f} s ---")
    if not rows:
        print("  no samples")
        return
    offs = sorted(abs(x - want) for _, x, want, _, _, _, _ in rows)
    typical = offs[len(offs) // 2]
    print(f"   t     light  grid   off   beat")
    for t, x, want, beat, prog, plink, ng in rows:
        off = x - want
        flag = "  <<<" if abs(off) > typical + 3 else ""
        print(f"  {t:6.3f}s  {x:4d}  {want:5.1f}  {off:+5.1f}   {beat}  "
              f"{'DECK' if plink else 'est'}{flag}")
    print(f"\n  the light is usually {typical:.1f} px from the grid's place")


if __name__ == "__main__":
    asyncio.run(main())

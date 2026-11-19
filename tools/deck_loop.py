"""
Capture a deck loop: the position, the light, and the speed.

The deck is in a loop; this watches three things at once:

  - the deck's position in the song (the board's source_32, which is the
    count modulo 32) - it should step BACKWARDS when the deck loops,
  - the light's position on the wall (the liveview),
  - the deck's speed field, straight off the air (the beat packets are
    broadcast, so a PC on the subnet sees them).

Usage:
    python tools/deck_loop.py [seconds]
"""
import asyncio
import json
import os
import socket
import struct
import sys
import time
import urllib.request

import websockets

HOST = os.environ.get("WLED_HOST", "192.168.6.249")
URL = f"ws://{HOST}/ws"
AR = f"http://{HOST}/json/ar"
PORT_BEAT = 50001


def get(url):
    with urllib.request.urlopen(url, timeout=8) as r:
        return json.loads(r.read().decode("utf-8"))


async def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0

    # The deck's own packets, on the air.
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", PORT_BEAT))
    s.setblocking(False)

    rows = []
    light = []
    t0 = time.time()

    async def poll():
        while time.time() - t0 < seconds:
            try:
                t = get(AR)["AudioReactive"]["tempo"]
                rows.append((time.time() - t0, t.get("source_32"), t.get("source_beat"),
                             t.get("source_bpm")))
            except Exception as e:
                rows.append((time.time() - t0, "err", str(e)[:40], None))
            await asyncio.sleep(0.2)

    async def live():
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
                w, h = raw[2], raw[3]
                px = raw[4:]
                best, bx = -1, None
                for i in range(w * h):
                    v = px[i * 3] + px[i * 3 + 1] + px[i * 3 + 2]
                    if v > best:
                        best, bx = v, i % w
                if bx is not None and best > 100:
                    light.append((time.time() - t0, bx))

    async def packets():
        while time.time() - t0 < seconds:
            try:
                data, _ = s.recvfrom(2048)
            except BlockingIOError:
                await asyncio.sleep(0.01)
                continue
            if len(data) >= 0x5D:
                raw = struct.unpack(">I", data[0x54:0x58])[0]
                signed = raw - (1 << 32) if raw & 0x80000000 else raw
                print(f"  {time.time()-t0:6.3f}s  BEAT  speed raw {raw}  signed {signed} "
                      f"ratio {signed/1048576.0:+.4f}  beat no {data[0x5C]}")

    await asyncio.gather(poll(), live(), packets())

    print(f"\n--- the deck's position (source_32) over {seconds:.0f} s ---")
    prev = None
    for t, s32, beat, bpm in rows:
        mark = ""
        if prev is not None and isinstance(s32, int) and isinstance(prev, int):
            d = (s32 - prev) % 32
            if d > 16:
                d -= 32
            mark = "  <<< BACKWARDS" if d < 0 else ""
        if isinstance(s32, int):
            print(f"  {t:6.3f}s  position {s32:3d}  beat in bar {beat}  bpm {bpm}{mark}")
        prev = s32

    print(f"\n--- the light on the wall ({len(light)} samples) ---")
    prev = None
    for t, x in light:
        if prev is not None and abs(x - prev) >= 1:
            print(f"  {t:6.3f}s  x {x:3d}  ({'back' if x < prev else 'forward'})")
        prev = x


if __name__ == "__main__":
    asyncio.run(main())

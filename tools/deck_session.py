"""
A long capture of everything the deck is doing, for as long as it is wanted.

Three sources, one log:

  PACKET  the deck's own beat packets, off the air (they are broadcast, so a
          PC on the subnet sees them): the beat number and the speed field,
          printed both ways - as unsigned and as signed, because a reverse
          would come out as a huge unsigned number.

  BOARD   the board's own numbers, polled: the deck's position in the song
          (source_32), the beat it is on, the grid's quality, the level and
          the drift.

  TEMPO   the tempo stream on the websocket: the beat, the progress through
          it, and whether the board thinks there is a beat at all.

  LIGHT   the light on the wall, from the liveview.

The board's HTTP is polled slowly on purpose - it wedges after a few dozen
requests - so the position is sampled every couple of seconds, which is
plenty for "did the music stop".

Usage:
    python tools/deck_session.py [minutes] [logfile]
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
SPEED_UNITY = 1048576.0


def get(url):
    with urllib.request.urlopen(url, timeout=8) as r:
        return json.loads(r.read().decode("utf-8"))


async def main():
    minutes = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
    out_path = sys.argv[2] if len(sys.argv) > 2 else "C:/Users/troys/AppData/Local/Temp/session.log"
    out = open(out_path, "w", buffering=1)
    t0 = time.time()
    end = t0 + minutes * 60

    def log(kind, **kw):
        out.write(f"{time.time()-t0:9.3f} {kind:7s} " +
                  " ".join(f"{k}={v}" for k, v in kw.items()) + "\n")

    # The deck's own packets.
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", PORT_BEAT))
    s.setblocking(False)

    async def packets():
        while time.time() < end:
            try:
                data, _ = s.recvfrom(2048)
            except BlockingIOError:
                await asyncio.sleep(0.005)
                continue
            if len(data) < 0x5D:
                continue
            raw = struct.unpack(">I", data[0x54:0x58])[0]
            signed = raw - (1 << 32) if raw & 0x80000000 else raw
            log("PACKET", player=data[0x21], beat=data[0x5C],
                raw=raw, signed=signed, ratio=round(signed / SPEED_UNITY, 5),
                bpm=round(struct.unpack(">H", data[0x5A:0x5C])[0] / 100.0, 2))

    async def board():
        while time.time() < end:
            try:
                ar = get(AR)["AudioReactive"]
                t = ar["tempo"]
                lv = ar.get("level", {})
                log("BOARD", pos=t.get("source_32"), beat=t.get("source_beat"),
                    q=round(t.get("quality", 0), 3),
                    db=round(lv.get("level_db", 0), 1),
                    drift=round(t.get("drift_ms", 0), 1),
                    live=t.get("live"), src=t.get("source"))
            except Exception as e:
                log("BOARD", err=str(e)[:60])
            await asyncio.sleep(2.0)

    async def tempo():
        last = None
        while time.time() < end:
            try:
                async with websockets.connect(URL, max_size=None) as ws:
                    await ws.send(json.dumps({"tempo": True}))
                    while time.time() < end:
                        raw = await asyncio.wait_for(ws.recv(), timeout=5)
                        if not isinstance(raw, str):
                            continue
                        try:
                            t = json.loads(raw).get("tempo")
                        except Exception:
                            continue
                        if not t:
                            continue
                        key = (t.get("beat"), t.get("plink"), t.get("live"))
                        if key != last:
                            last = key
                            log("TEMPO", beat=t.get("beat"),
                                prog=round(t.get("prog", 0), 3),
                                bpm=t.get("bpm"), live=t.get("live"),
                                plink=t.get("plink"), lock=t.get("lock"))
            except Exception as e:
                log("TEMPO", err=str(e)[:60])
                await asyncio.sleep(1)

    async def light():
        while time.time() < end:
            try:
                async with websockets.connect(URL, max_size=None) as ws:
                    await ws.send(json.dumps({"lv": True}))
                    while time.time() < end:
                        raw = await asyncio.wait_for(ws.recv(), timeout=5)
                        if isinstance(raw, str):
                            continue
                        if len(raw) < 4 or raw[0:1] != b"L":
                            continue
                        w, h = raw[2], raw[3]
                        px = raw[4:]
                        best, bx = -1, None
                        for i in range(w * h):
                            v = px[i * 3] + px[i * 3 + 1] + px[i * 3 + 2]
                            if v > best:
                                best, bx = v, i % w
                        if bx is not None and best > 100:
                            log("LIGHT", x=bx)
            except Exception as e:
                log("LIGHT", err=str(e)[:60])
                await asyncio.sleep(1)

    out.write(f"# the capture started at {time.ctime()}, {minutes:.1f} minutes\n")
    await asyncio.gather(packets(), board(), tempo(), light())
    out.write(f"# the capture ended at {time.ctime()}\n")
    out.close()
    print(f"the capture is in {out_path}")


if __name__ == "__main__":
    asyncio.run(main())

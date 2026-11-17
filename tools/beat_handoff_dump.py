"""Dump the Bar Mover's raw position, so the handoff can be seen directly."""
import asyncio, json, os, sys, time, urllib.request, websockets

HOST = os.environ.get("WLED_HOST", "192.168.6.249")
WS = f"ws://{HOST}/ws"
STATE = f"http://{HOST}/json/state"


def post_state(body):
    req = urllib.request.Request(
        STATE, data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"}, method="POST")
    return urllib.request.urlopen(req, timeout=8).read()


def head_of(w, px):
    num = den = 0.0
    for i, (r, g, b) in enumerate(px[:w]):
        v = r + g + b
        if v > 8:
            num += i * v
            den += v
    return None if den <= 0 else num / den


async def main(seconds):
    async with websockets.connect(WS, max_size=None, open_timeout=5) as ws:
        await ws.send(json.dumps({"lv": True}))
        t0 = time.time()
        while time.time() - t0 < seconds:
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=5)
            except asyncio.TimeoutError:
                continue
            if isinstance(raw, str):
                continue
            w, h = raw[2], raw[3]
            px = [(raw[4 + i * 3], raw[5 + i * 3], raw[6 + i * 3]) for i in range(w * h)]
            x = head_of(w, px)
            if x is not None:
                print(f"{time.time()-t0:7.3f}  {x:7.3f}")

post_state({"seg": [{"id": 0, "fx": 237, "sx": 0, "ix": 0,
                     "col": [[0, 96, 255, 0], [0, 0, 0, 0], [0, 0, 0, 0]]}]})
time.sleep(0.6)
asyncio.run(main(float(sys.argv[1]) if len(sys.argv) > 1 else 20.0))

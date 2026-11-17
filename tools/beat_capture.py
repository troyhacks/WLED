"""Capture the Bar Mover's path to a file, so it can be analysed offline.

    python tools/beat_capture.py out.json [seconds]
"""
import asyncio, json, os, sys, time, urllib.request, websockets

HOST = os.environ.get("WLED_HOST", "192.168.6.249")
WS = f"ws://{HOST}/ws"
STATE = f"http://{HOST}/json/state"


def get(url):
    with urllib.request.urlopen(url, timeout=8) as r:
        return json.loads(r.read().decode("utf-8"))


def post_state(body):
    req = urllib.request.Request(
        STATE, data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=8) as r:
        return json.loads(r.read().decode("utf-8"))


def head_of(w, px):
    num = den = 0.0
    for i, (r, g, b) in enumerate(px[:w]):
        v = r + g + b
        if v > 8:
            num += i * v
            den += v
    return None if den <= 0 else num / den


async def capture(seconds, frames):
    t0 = time.time()
    while time.time() - t0 < seconds:
        try:
            async with websockets.connect(WS, max_size=None, open_timeout=5) as ws:
                await ws.send(json.dumps({"lv": True}))
                last = time.time()
                while time.time() - t0 < seconds:
                    if time.time() - last > 2.5:
                        break
                    try:
                        raw = await asyncio.wait_for(ws.recv(), timeout=1.0)
                    except asyncio.TimeoutError:
                        continue
                    if isinstance(raw, str):
                        continue
                    last = time.time()
                    w, h = raw[2], raw[3]
                    px = [(raw[4 + i * 3], raw[5 + i * 3], raw[6 + i * 3])
                          for i in range(w * h)]
                    x = head_of(w, px)
                    if x is not None:
                        frames.append((time.time() - t0, x, w))
        except Exception:
            await asyncio.sleep(0.3)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "frames.json"
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
    post_state({"seg": [{"id": 0, "fx": 237, "sx": 0, "ix": 0,
                         "o1": False, "o2": False, "o3": False,
                         "col": [[0, 96, 255, 0], [0, 0, 0, 0], [0, 0, 0, 0]]}]})
    time.sleep(0.8)
    seg = get(STATE)["seg"][0]
    print("the segment now:", {k: seg.get(k) for k in
                               ("fx", "sx", "ix", "o1", "o2", "o3")})
    frames = []
    asyncio.run(capture(seconds, frames))
    with open(out, "w") as f:
        json.dump(frames, f)
    print(f"{len(frames)} frames -> {out}")


if __name__ == "__main__":
    main()

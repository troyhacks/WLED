"""The Beat Sweep's CYCLER: when the sub-effect changes, and on what bar."""
import asyncio, json, os, sys, time, urllib.request
import websockets
HOST = os.environ.get("WLED_HOST", "192.168.6.249")
URL = f"ws://{HOST}/ws"
STATE = f"http://{HOST}/json/state"

def get(u):
    with urllib.request.urlopen(u, timeout=10) as r: return json.loads(r.read().decode())
def post(b):
    q = urllib.request.Request(STATE, data=json.dumps(b).encode(),
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(q, timeout=12) as r: return json.loads(r.read().decode())
def dec(f):
    if len(f) < 4 or f[0:1] != b"L": return None
    w, h = f[2], f[3]
    return w, h, [(f[4+i*3], f[5+i*3], f[6+i*3]) for i in range(w*h) if 6+i*3 <= len(f)]

async def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 40.0
    before = get(STATE)["seg"][0]
    saved = {k: before.get(k) for k in ("fx","sx","ix","c1","c2","c3","o1","o2","o3","col","pal","m12","si")}
    post({"seg":[{"id":0,"fx":241,"sx":128,"ix":128,"c1":96,"c3":12,
                  "o1":True,"o2":False,"o3":False,
                  "col":[[0,0,255,0],[0,0,0,0],[255,255,0,0]]}]})
    time.sleep(1.0)
    rows, tempos = [], []
    try:
        async with websockets.connect(URL, max_size=None) as ws:
            await ws.send(json.dumps({"lv": True}))
            t0 = time.time()
            while time.time() - t0 < seconds:
                try: raw = await asyncio.wait_for(ws.recv(), timeout=seconds)
                except asyncio.TimeoutError: break
                if isinstance(raw, str):
                    try:
                        d = json.loads(raw)
                        tm = d.get("tempo") if isinstance(d, dict) else None
                        if tm: tempos.append((time.time()-t0, tm))
                    except Exception: pass
                    continue
                d = dec(raw)
                if not d: continue
                w, h, px = d
                lit = [(i%w, i//w) for i,(r,g,b) in enumerate(px) if r+g+b > 24]
                rows.append((time.time()-t0, len(lit),
                             max((x for x,_ in lit), default=0)-min((x for x,_ in lit), default=0),
                             sum(x for x,_ in lit)/len(lit) if lit else -1))
    finally:
        try: post({"seg":[{"id":0, **saved}]})
        except Exception as e: print("restore failed:", e)
    print(f"--- {len(rows)} frames, {len(tempos)} tempo frames ---")
    # A sub-effect change rewrites the whole strip: the lit count or the
    # spread jumps, not the light's position.
    print("  strip changes (the cycler):")
    prev = None
    for t, n, spread, pos in rows:
        key = (n > 20, spread > 25)
        if prev is not None and key != prev[0]:
            print("    %7.3f s  lit %4d spread %3d  light x %5.1f" % (t, n, spread, pos))
        prev = (key, t)
    if tempos:
        print("  the tempo frames (beat in bar / count):")
        for t, tm in tempos[::max(1, len(tempos)//12)]:
            print("    %7.3f s  %s" % (t, {k: tm.get(k) for k in
                  ("bpm","beatInBar","beatCount","beatProgress","fromProLink") if k in tm}))
asyncio.run(main())

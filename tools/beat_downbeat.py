"""
The downbeat button: one state with the estimator, two with a deck.

  python tools/beat_downbeat.py            # the estimator
  python tools/beat_downbeat.py --deck     # with a fake Pro DJ Link deck

It watches the tempo frame over the websocket - the same frame the GUI
draws the button from - and presses the button the same way the GUI
does: over the websocket, with {setDownbeat:true} or
{clearDownbeat:true}.
"""
import argparse
import asyncio
import json
import os
import subprocess
import sys
import time
import urllib.request

import websockets

HOST = os.environ.get("WLED_HOST", "192.168.6.249")
WS = f"ws://{HOST}/ws"
AR = f"http://{HOST}/json/ar"

HERE = os.path.dirname(os.path.abspath(__file__))
FAKE = os.path.join(HERE, "..", "usermods", "audioreactive", "prolink_fake.py")


def tempo_now():
    with urllib.request.urlopen(AR, timeout=8) as r:
        return json.loads(r.read().decode("utf-8"))["AudioReactive"]["tempo"]


async def run(deck):
    frames = []
    t0 = time.time()

    async def watch():
        async with websockets.connect(WS, max_size=None, open_timeout=5) as ws:
            await ws.send(json.dumps({"tempo": True}))
            while time.time() - t0 < 40:
                try:
                    raw = await asyncio.wait_for(ws.recv(), timeout=5)
                except asyncio.TimeoutError:
                    continue
                if isinstance(raw, str):
                    try:
                        j = json.loads(raw)
                    except Exception:
                        continue
                    if "tempo" in j:
                        frames.append((time.time() - t0, j["tempo"]))
                await asyncio.sleep(0.05)

    async def press(when, cmd):
        await asyncio.sleep(max(0.0, when - (time.time() - t0)))
        print(f"  -> pressing the button: {cmd}")
        async with websockets.connect(WS, max_size=None, open_timeout=5) as ws:
            await ws.send(json.dumps({"AudioReactive": cmd}))
            await asyncio.sleep(0.4)

    proc = None
    if deck:
        cmd = [sys.executable, "-u", FAKE, "--host", HOST, "--watch", "--delay", "40"]
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                stderr=subprocess.STDOUT)
        print("  the fake deck is running")
        time.sleep(6)

    try:
        await asyncio.gather(
            watch(),
            press(8.0, {"setDownbeat": True}),
            press(16.0, {"clearDownbeat": True}),
            press(22.0, {"setDownbeat": True}),
        )
    finally:
        if proc:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except Exception:
                proc.kill()

    print("\n--- the tempo frame, as the GUI sees it ---")
    last = None
    for t, f in frames:
        key = (f.get("plink"), f.get("dbset"), f.get("dbo"))
        if key != last:
            print(f"  t={t:5.1f}s  plink={f.get('plink')} dbset={f.get('dbset')} "
                  f"dbo={f.get('dbo')} beat={f.get('beat')}")
            last = key

    print("\n--- the board's own numbers ---")
    t = tempo_now()
    for k in ("source", "source_bpm", "source_beat", "source_32",
              "downbeat_set", "downbeat_known", "deck_override",
              "deck_downbeat", "beat_in_measure", "beat_in_measure32"):
        print(f"  {k:20s} {t.get(k)}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--deck", action="store_true")
    args = ap.parse_args()
    asyncio.run(run(args.deck))

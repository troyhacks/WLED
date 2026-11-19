"""
Watch the tempo stream: does the beat go BACKWARDS, and does the source
flicker?

The tempo frames arrive at 25 Hz on the websocket and carry the source
flag (plink), the beat in the bar, the progress through it and the drift.
A source that flickers between the deck and the estimator makes the
publish layer's handoff correction fire and be released, over and over -
and that is what "runs for a bit and then reverses" looks like.

Usage:
    python tools/tempo_stream.py [seconds]
"""
import asyncio
import json
import os
import sys
import time

import websockets

HOST = os.environ.get("WLED_HOST", "192.168.6.249")
URL = f"ws://{HOST}/ws"


async def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0
    async with websockets.connect(URL, max_size=None) as ws:
        await ws.send(json.dumps({"tempo": True}))
        t0 = time.time()
        last = None
        changes = []
        while time.time() - t0 < seconds:
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=seconds)
            except asyncio.TimeoutError:
                break
            if not isinstance(raw, str):
                continue
            try:
                j = json.loads(raw)
            except Exception:
                continue
            t = j.get("tempo")
            if not t:
                continue
            key = (t.get("plink"), t.get("beat"))
            if key != last:
                changes.append((time.time() - t0, t.get("plink"), t.get("beat"),
                                t.get("prog"), t.get("drift")))
                last = key
        print(f"--- {len(changes)} changes in {seconds:.0f} s ---")
        for c in changes:
            src = "DECK" if c[1] else "est "
            print(f"  {c[0]:7.3f}s  {src}  beat {c[2]}  prog {c[3]}  drift {c[4]}")

        # The beat's order: any step that is not +1 (mod 4) is a wrong way.
        beats = [c[2] for c in changes if c[2]]
        bad = []
        for a, b in zip(beats, beats[1:]):
            d = (b - a) % 4
            if d != 1:
                bad.append((a, b, d))
        print(f"\n  the beat went {beats[:24]}")
        print(f"  steps that were not +1: {bad if bad else 'none'}")


if __name__ == "__main__":
    asyncio.run(main())

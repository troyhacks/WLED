"""
Watch the PHRASE effect's step and report the sequence.

The Phrase effect lights one of sixteen steps, one per beat.  On a bar too
short for a ring the sixteen marks are laid along the width, one in each
sixteenth, so the lit step can be read straight off the liveview.

What matters is the SEQUENCE: one step per beat, in order, with no skips
and no going backwards.  A gap in the sequence means the beat count jumped
(two beats counted as one); a repeat means it stalled; a step backwards
means the count and the phase are on different grids.

Usage:
    python tools/beat_steps.py [seconds] [fx]

    fx 236 = Phrase (default), 235 = Beat Counter

The segment is set up for the test and RESTORED afterwards - the board's
own segment settings are put back exactly as they were.
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
            lit = [(i % w, i // w) for i, (r, g, b) in enumerate(px) if r + g + b > 24]
            out.append((time.time() - t0, w, h, lit))


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0
    fx = int(sys.argv[2]) if len(sys.argv) > 2 else 236

    before = get(STATE)["seg"][0]
    saved = {k: before.get(k) for k in ("fx", "sx", "ix", "c1", "c2", "c3",
                                        "o1", "o2", "o3", "col", "pal", "m12", "si")}
    print("saved:", {k: saved[k] for k in ("fx", "sx", "ix", "o1", "o2", "o3")})

    post_state({"seg": [{"id": 0, "fx": fx, "sx": 128, "ix": 96,
                         "col": [[0, 0, 255, 0], [0, 0, 0, 0], [255, 255, 255, 0]]}]})
    time.sleep(0.6)

    frames = []
    try:
        asyncio.run(watch(seconds, frames))
    finally:
        post_state({"seg": [{"id": 0, **saved}]})

    # The lit marks, and the step they mean.
    marks = []
    prev = None
    for t, w, h, lit in frames:
        key = tuple(lit)
        if key == prev:
            continue
        prev = key
        if not lit:
            continue
        # On a two-row bar the Phrase's steps are single pixels along the
        # width: step s sits at x = s * w / 16.
        xs = sorted(x for (x, y) in lit)
        step = round(xs[0] * 16.0 / w) % 16
        marks.append((t, step, xs))

    print(f"\n--- {len(marks)} changes over {seconds:.0f} s ---")
    prev = None
    gaps = []
    for t, step, xs in marks:
        d = "" if prev is None else f"  step {step - prev:+d}"
        print(f"  {t:7.3f}s  step {step:2d}  at x {xs}{d}")
        if prev is not None:
            gaps.append((step - prev) % 16)
        prev = step

    if gaps:
        from collections import Counter
        c = Counter(gaps)
        print("\n  the steps between changes:", dict(sorted(c.items())))
        bad = {k: v for k, v in c.items() if k not in (1,)}
        print("  anything but +1:", bad if bad else "none")

    t = get(AR)["AudioReactive"]["tempo"]
    print(f"\n  the board's tempo: {t.get('bpm')} BPM, source {t.get('source')}, "
          f"beat in bar {t.get('beat_in_bar')}, source beat {t.get('source_beat')}, "
          f"beat count {t.get('beat_count')}, source 32 {t.get('source_32')}")


if __name__ == "__main__":
    main()

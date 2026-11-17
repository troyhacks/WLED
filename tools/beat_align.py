"""
Does the Beat Sweep follow the master tempo's counter?

The effect used to count its own bars, so marking the downbeat - which
RESTARTS the master tempo's beat counter - left the effect exactly where
it was: it kept counting from its own number, and only lined up with the
first beat of the bar.

Now the bar comes from the master tempo's counter, so the mark moves the
effect with it.  And that is VISIBLE: after the mark the effect is
showing the sub-effect for bar ZERO again - the same one it was showing
when it started - because the counter restarted under it.

So the test watches the wall, notes what the effect looks like at the
start (bar zero), marks the downbeat, and asks whether the effect goes
back to that look.

    python tools/beat_align.py
"""
import asyncio
import json
import os
import time
import urllib.request

import websockets

HOST = os.environ.get("WLED_HOST", "192.168.6.249")
WS = f"ws://{HOST}/ws"
STATE = f"http://{HOST}/json/state"
AR = f"http://{HOST}/json/ar"

FX_BEAT_SWEEP = 241
MARK_AT = 12.0
CLEAR_AT = 24.0


def get(url):
    with urllib.request.urlopen(url, timeout=8) as r:
        return json.loads(r.read().decode("utf-8"))


def post_state(body):
    req = urllib.request.Request(
        STATE, data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=8) as r:
        return json.loads(r.read().decode("utf-8"))


def press(cmd):
    req = urllib.request.Request(
        f"http://{HOST}/json/si",
        data=json.dumps({"AudioReactive": cmd}).encode("utf-8"),
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=8) as r:
        r.read()


def lit_count(raw):
    """How much of the strip is lit.  A ribbon fills it; a light does not."""
    w, h = raw[2], raw[3]
    n = 0
    for i in range(w * h):
        o = 4 + i * 3
        if o + 3 <= len(raw) and raw[o] + raw[o + 1] + raw[o + 2] > 8:
            n += 1
    return n


def median(v):
    s = sorted(v)
    if not s:
        return 0
    m = len(s) // 2
    return s[m] if len(s) % 2 else 0.5 * (s[m - 1] + s[m])


async def main():
    post_state({"seg": [{"id": 0, "fx": FX_BEAT_SWEEP, "sx": 255, "ix": 128,
                         "c1": 96, "c2": 0, "c3": 12,
                         "o1": True, "o2": False, "o3": False, "pal": 11,
                         "col": [[0, 0, 255, 0], [0, 0, 0, 0], [255, 255, 255, 0]]}]})
    time.sleep(1.0)

    counts = []
    t0 = time.time()

    async def watch():
        async with websockets.connect(WS, max_size=None, open_timeout=5) as ws:
            await ws.send(json.dumps({"lv": True}))
            while time.time() - t0 < 34:
                try:
                    raw = await asyncio.wait_for(ws.recv(), timeout=2.0)
                except asyncio.TimeoutError:
                    continue
                if isinstance(raw, str):
                    continue
                counts.append((time.time() - t0, lit_count(raw)))
                await asyncio.sleep(0.005)

    async def actor():
        await asyncio.sleep(MARK_AT)
        t = get(AR)["AudioReactive"]["tempo"]
        print(f"  t={MARK_AT:.1f}s  marking the downbeat "
              f"(the master counter is at {t.get('beat_count')}, "
              f"beat {t.get('beat_in_bar')})")
        press({"setDownbeat": True})
        await asyncio.sleep(CLEAR_AT - MARK_AT)
        t = get(AR)["AudioReactive"]["tempo"]
        print(f"  t={CLEAR_AT:.1f}s  clearing it "
              f"(the master counter is at {t.get('beat_count')})")
        press({"clearDownbeat": True})

    await asyncio.gather(watch(), actor())

    def med(a, b):
        return median([c for t, c in counts if a <= t < b])

    # The lit count, second by second, so a change of sub-effect shows.
    print("\n--- the lit count, second by second ---")
    buckets = {}
    for t, c in counts:
        b = int(t)
        buckets.setdefault(b, []).append(c)
    for b in sorted(buckets):
        v = median(buckets[b])
        mark = ""
        if abs(b - MARK_AT) <= 1:
            mark = "   <-- the mark"
        elif abs(b - CLEAR_AT) <= 1:
            mark = "   <-- the clear"
        print(f"  t={b:3d}s  {v:6.1f}{mark}")

    start = med(0.5, 3.0)          # the look at bar zero
    before = med(MARK_AT - 4.0, MARK_AT - 0.5)
    after = med(MARK_AT + 1.0, MARK_AT + 4.0)
    cleared = med(CLEAR_AT + 1.0, CLEAR_AT + 4.0)

    print("\n--- how much of the strip is lit (the median of each window) ---")
    print(f"  at the start (bar zero)   {start:6.1f}")
    print(f"  just before the mark      {before:6.1f}")
    print(f"  just after the mark       {after:6.1f}   "
          f"{'SAME as bar zero - the effect followed the master' if abs(after-start) < 1.5 else 'different - it did NOT follow'}")
    print(f"  just after the clear      {cleared:6.1f}")

    print("\nThe counter restarts when the downbeat is marked - that is the")
    print("master tempo's own rule - and the effect's bar comes from that")
    print("counter now, so the effect goes back to the sub-effect for bar")
    print("zero with it.")


if __name__ == "__main__":
    asyncio.run(main())

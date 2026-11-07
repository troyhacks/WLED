"""Read the board's live view and print it as ASCII.

The 2D live view is a BINARY websocket frame: a four byte header
(id, version, width, height) and then width*height RGB triples, scaled
down by the PPA.  320x180 comes back as 64x36.

    "C:/Users/troys/.platformio/penv/Scripts/python.exe" tools/ar_sim/_liveview.py

Add --url, --frames, --wait.  The output is one character per pixel:
' ' is black, '#' is bright, '.' is in between.
"""

import argparse
import asyncio
import sys

import websockets


async def grab(url, frames, wait):
    out = []
    async with websockets.connect(url, max_size=None) as ws:
        await ws.send('{"lv":true}')
        deadline = asyncio.get_event_loop().time() + wait
        while len(out) < frames:
            left = deadline - asyncio.get_event_loop().time()
            if left <= 0:
                break
            try:
                msg = await asyncio.wait_for(ws.recv(), timeout=left)
            except asyncio.TimeoutError:
                break
            if isinstance(msg, (bytes, bytearray)):
                out.append(bytes(msg))
    return out


def show(frame):
    if len(frame) < 4:
        return
    w, h = frame[2], frame[3]
    px = frame[4:]
    print("live view %dx%d" % (w, h))
    peak = 0
    for y in range(h):
        row = []
        for x in range(w):
            i = (y * w + x) * 3
            if i + 2 >= len(px):
                row.append(' ')
                continue
            v = max(px[i], px[i + 1], px[i + 2])
            if v > peak:
                peak = v
            row.append('#' if v > 160 else ('.' if v > 8 else ' '))
        print(''.join(row))
    print("peak brightness %d" % peak)


def ink(frames):
    """The brightness of each row, averaged over the frames.

    The live view is scaled right down, so a glyph is a pixel or two and
    cannot be READ.  What can be measured is how much ink there is: a
    macro that expanded to two digits puts far less on the screen than a
    macro left as five literal characters.
    """
    if not frames:
        return []
    w, h = frames[-1][2], frames[-1][3]
    tot = [0.0] * h
    n = 0
    for f in frames:
        if len(f) < 4 or f[2] != w or f[3] != h:
            continue
        px = f[4:]
        n += 1
        for y in range(h):
            s = 0
            for x in range(w):
                i = (y * w + x) * 3
                if i + 2 < len(px):
                    s += max(px[i], px[i + 1], px[i + 2])
            tot[y] += s / float(w)
    if not n:
        return []
    return [t / n for t in tot]


def bright(frames, thr=150):
    """How many pixels are brighter than the background, per frame.

    The scrolling text draws WHITE on a flat background, so the number of
    bright pixels is the number of glyphs on the screen - which is how a
    macro that expanded to two digits can be told from one that was left
    as five literal characters, at a resolution where no glyph can be read.
    """
    counts = []
    for f in frames:
        if len(f) < 4:
            continue
        w, h = f[2], f[3]
        px = f[4:]
        c = 0
        for i in range(0, min(len(px), w * h * 3), 3):
            if max(px[i], px[i + 1], px[i + 2]) > thr:
                c += 1
        counts.append(c)
    if not counts:
        return 0.0, 0
    return sum(counts) / float(len(counts)), len(counts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="ws://192.168.6.249/ws")
    ap.add_argument("--frames", type=int, default=3)
    ap.add_argument("--wait", type=float, default=8.0)
    ap.add_argument("--ink", action="store_true")
    ap.add_argument("--bright", action="store_true")
    args = ap.parse_args()
    frames = asyncio.run(grab(args.url, args.frames, args.wait))
    if not frames:
        print("no live view frames")
        return 1
    if args.bright:
        m, n = bright(frames)
        print("%d frames, bright pixels: mean %.2f" % (n, m))
        return 0
    if args.ink:
        rows = ink(frames)
        print("%d frames, row brightness:" % len(frames))
        for y, v in enumerate(rows):
            if v > 0.5:
                print("  row %2d  %6.2f" % (y, v))
        print("  total %6.2f" % sum(rows))
        return 0
    for f in frames[-1:]:
        show(f)
    return 0


if __name__ == "__main__":
    sys.exit(main())

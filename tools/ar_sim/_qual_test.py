"""Prove that the #QUAL macro expands, on the live board.

The live view is scaled to 60x33 - three sixteenths - so a glyph is one
pixel and cannot be READ.  But it can be COUNTED: the scrolling text
draws white on a flat background, and the number of bright pixels is the
number of glyphs on the screen.  A macro that expanded to two digits
therefore puts far fewer bright pixels out than one left as five literal
characters.

The other segment's effect is animated and full of bright pixels, so it
is switched off for the duration and put back afterwards, and every
measurement waits for the previous text's TRAIL to fade before it reads
anything.

    "C:/Users/troys/.platformio/penv/Scripts/python.exe" tools/ar_sim/_qual_test.py
"""

import asyncio
import json
import sys
import time
import urllib.request

import _liveview as LV

BASE = "http://192.168.6.249"
WS = "ws://192.168.6.249/ws"
TEXT_SEG = 1
OTHER_SEG = 0
ORIGINAL = "#BPM - #BEAT"
FRAMES = 12
SETTLE = 6.0


def post(obj):
    body = json.dumps(obj).encode()
    req = urllib.request.Request(BASE + "/json/state", data=body,
                                 headers={"Content-Type": "application/json"})
    urllib.request.urlopen(req, timeout=10).read()


def measure(name):
    post({"seg": [{"id": TEXT_SEG, "n": name}]})
    time.sleep(SETTLE)
    got = asyncio.run(LV.grab(WS, FRAMES, 12.0))
    m, n = LV.bright(got)
    print("  %-16s frames=%3d  bright pixels/frame %6.2f" % (repr(name), n, m))
    return m


def main():
    cases = ["", "35", "12345", "#QUAL", "#BPM", "#ZZZZ"]
    try:
        post({"seg": [{"id": OTHER_SEG, "on": False}]})
        time.sleep(2.0)
        res = {}
        for name in cases:
            res[name] = measure(name)
        print("  --- second pass, the trails are gone ---")
        for name in cases:
            res[name] = measure(name)
    finally:
        post({"seg": [{"id": OTHER_SEG, "on": True},
                      {"id": TEXT_SEG, "n": ORIGINAL}]})
    print()
    base = res[""]
    for name in cases:
        print("  %-16s %6.2f   (%+.2f over the empty text)"
              % (repr(name), res[name], res[name] - base))
    return 0


if __name__ == "__main__":
    sys.exit(main())

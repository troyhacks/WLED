"""Look at the text region of the live view, as ASCII, for a given text.

    "C:/Users/troys/.platformio/penv/Scripts/python.exe" tools/ar_sim/_qual_shot.py "#QUAL"
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


def post(obj):
    body = json.dumps(obj).encode()
    req = urllib.request.Request(BASE + "/json/state", data=body,
                                 headers={"Content-Type": "application/json"})
    urllib.request.urlopen(req, timeout=10).read()


def main():
    text = sys.argv[1] if len(sys.argv) > 1 else "#QUAL"
    try:
        post({"seg": [{"id": OTHER_SEG, "on": False}]})
        post({"seg": [{"id": TEXT_SEG, "n": text}]})
        time.sleep(2.0)
        got = asyncio.run(LV.grab(WS, 6, 8.0))
    finally:
        post({"seg": [{"id": OTHER_SEG, "on": True},
                      {"id": TEXT_SEG, "n": ORIGINAL}]})
    if not got:
        print("no frames")
        return 1
    f = got[-1]
    w, h = f[2], f[3]
    px = f[4:]
    print("live view %dx%d, text %r" % (w, h, text))
    for y in range(h):
        row = []
        for x in range(w):
            i = (y * w + x) * 3
            v = max(px[i], px[i + 1], px[i + 2]) if i + 2 < len(px) else 0
            row.append('#' if v > 160 else ('.' if v > 8 else ' '))
        if '#' in row or '.' in row:
            print("%2d |%s|" % (y, ''.join(row)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

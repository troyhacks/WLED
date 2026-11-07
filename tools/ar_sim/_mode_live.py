#!/usr/bin/env python3
"""The live check of the two modes.

Waits for the board, reads the tempo object, flips the mode with the same
POST the info pane's button makes, and reads it back.
"""
import json
import sys
import time
import urllib.request

BASE = "http://192.168.6.249"


def get(path):
    with urllib.request.urlopen(BASE + path, timeout=10) as r:
        return json.load(r)


def post(path, body):
    req = urllib.request.Request(BASE + path,
                                 data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"},
                                 method="POST")
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.status


def wait_board(timeout=150):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            get("/json/info")
            print("board up after %.0f s" % (time.time() - t0), flush=True)
            return True
        except Exception:
            time.sleep(3)
    return False


def mode_of():
    t = get("/json/ar")["AudioReactive"]["tempo"]
    return t


def main():
    if not wait_board():
        print("the board never came up")
        return 1
    time.sleep(5)

    t = mode_of()
    print("mode %s (%s)  bpm %.3f  score %.3f  quant %s"
          % (t.get("mode"), t.get("mode_name"), t.get("bpm", 0),
             t.get("score", 0), t.get("quant")), flush=True)

    # The info pane's rows.
    try:
        info = get("/json/info")
        ar = info.get("AudioReactive", {})
        for row in ar:
            if "mode" in str(row).lower() or "downbeat" in str(row).lower():
                print("info row: %-20s %s" % (row, ar[row]))
    except Exception as e:
        print("info read failed:", e)

    # PLAYLIST.
    st = post("/json/state", {"AudioReactive": {"playlistMode": True}})
    time.sleep(1.5)
    t = mode_of()
    print("after playlistMode -> HTTP %s  mode %s (%s)"
          % (st, t.get("mode"), t.get("mode_name")), flush=True)

    # DJ SET.
    st = post("/json/state", {"AudioReactive": {"djSetMode": True}})
    time.sleep(1.5)
    t = mode_of()
    print("after djSetMode    -> HTTP %s  mode %s (%s)"
          % (st, t.get("mode"), t.get("mode_name")), flush=True)

    # The rate is still the rate, in both modes.
    for i in range(6):
        t = mode_of()
        print("   bpm %10.5f  score %.3f  beat %d  valid %d"
              % (t.get("bpm", 0), t.get("score", 0), t.get("beat_in_bar", 0),
                 1 if t.get("valid") else 0), flush=True)
        time.sleep(1.0)
    return 0


if __name__ == "__main__":
    sys.exit(main())

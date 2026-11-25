#!/usr/bin/env python
"""
Test the tempo verbs.

The verbs are the console's tempo keys:

    h    take the estimate: the BPM
    H    take the estimate: the BPM and the grid
    i    take the estimate: the grid
    [    the bar's numbering: back a beat
    ]    the bar's numbering: forward a beat
    <    the hint: look DOWN for a better match
    >    the hint: look UP for a better match

Opening COM4 resets the board (the USB-UART reset), so this waits for
the board to come back and for the tracker to lock before it presses
anything.  Each key is watched twice: the state over HTTP before and
after, and the console lines the key printed.

Usage:  python tools/verb_test.py [the keys to press]
"""
import json
import os
import sys
import threading
import time
import urllib.request

import serial

HOST = os.environ.get("WLED_HOST", "http://192.168.6.249")
PORT = "COM4"
KEYS = sys.argv[1:] or ["H", "h", "i", "]", "[", "]", "<", ">"]

lines = []
stop = False


def reader(ser):
    buf = b""
    while not stop:
        try:
            chunk = ser.read(4096)
        except Exception:
            break
        if chunk:
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                t = time.time()
                lines.append((t, line.decode("utf-8", "replace").rstrip("\r")))


def tempo():
    with urllib.request.urlopen(HOST + "/json/ar", timeout=10) as f:
        d = json.loads(f.read())
    return d.get("AudioReactive", {}).get("tempo", {})


def wait_for_board(timeout=240):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with urllib.request.urlopen(HOST + "/json/info", timeout=4) as f:
                d = json.loads(f.read())
            print("  the board is back: uptime=%s  (%.0f s to answer)"
                  % (d.get("uptime"), time.time() - t0), flush=True)
            return True
        except Exception:
            time.sleep(2)
    return False


def wait_for_lock(timeout=180):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            t = tempo()
            if t.get("valid"):
                print("  the tracker locked after %.0f s" % (time.time() - t0),
                      flush=True)
                return t
        except Exception:
            pass
        time.sleep(2)
    return {}


KEYS_OF_INTEREST = ("bpm", "beat_in_bar", "beat_in_measure", "beat_count",
                    "bar_rot", "bar_rot_gen", "hint", "hint_bpm", "score",
                    "downbeat_set", "source")


def show(t):
    if not t:
        return "no tempo"
    return " ".join("%s=%s" % (k, t.get(k)) for k in KEYS_OF_INTEREST)


def main():
    ser = serial.Serial(PORT, 115200, timeout=0.2)
    ser.dtr = False
    ser.rts = False
    th = threading.Thread(target=reader, args=(ser,), daemon=True)
    th.start()

    print("COM4 is open; the board resets when the port opens.", flush=True)
    if not wait_for_board():
        print("the board did not come back")
        print("--- the console said: ---")
        for _, l in lines[-80:]:
            if l.strip():
                print("  " + l.strip())
        return 1
    print("the board is back", flush=True)
    t = wait_for_lock()
    print("the tracker: %s" % show(t), flush=True)
    if not t.get("valid"):
        print("the tracker never locked - the verbs would say so")
        return 1

    for key in KEYS:
        before = tempo()
        mark = len(lines)
        ser.write(key.encode())
        time.sleep(2.5)
        after = tempo()
        print("\n=== the key %r ===" % key, flush=True)
        changed = []
        for k in KEYS_OF_INTEREST:
            if before.get(k) != after.get(k):
                changed.append("%s: %s -> %s" % (k, before.get(k), after.get(k)))
        print("  the state: %s" % ("; ".join(changed) if changed else "unchanged"),
              flush=True)
        for _, l in lines[mark:]:
            if l.strip():
                print("  console: %s" % l.strip(), flush=True)

    print("\n=== the last state ===")
    print("  %s" % show(tempo()))
    stop = True
    time.sleep(0.3)
    ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

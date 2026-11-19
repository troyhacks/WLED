"""
stagelinq_watch_board.py - watch the board follow the Denon deck.

Polls the board's /json/ar and prints what it is doing, so the board and
the deck can be watched side by side with tools/stagelinq_probe.py.

The board's web server does not like being hammered, so this keeps the
request count low: one GET every couple of seconds, and it stops on its
own after the time asked for.

    python tools/stagelinq_watch_board.py --seconds 60
"""

import argparse
import json
import time
import urllib.request

BOARD = "http://192.168.6.249"


def get(url):
    with urllib.request.urlopen(url, timeout=8) as r:
        return json.loads(r.read().decode())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=BOARD)
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--interval", type=float, default=2.0)
    args = ap.parse_args()

    print("%-8s %-11s %7s %5s %5s %6s  %s" %
          ("time", "source", "bpm", "beat", "bar", "beat32", "stageLinq"))
    print("   (beats_elapsed is the board's own absolute beat)")
    end = time.monotonic() + args.seconds
    n = 0
    while time.monotonic() < end:
        try:
            d = get(args.host + "/json/ar")
        except Exception as e:
            print("%-8s  ERROR %s" % (time.strftime("%H:%M:%S"), e))
            time.sleep(args.interval)
            continue
        n += 1
        t = d.get("AudioReactive", {}).get("tempo", {})
        s = d.get("StageLinq", {})
        print("%-8s %-11s %7.2f %5s %5s %6s  link=%s up=%s/%s frames=%s abs=%s %s" % (
            time.strftime("%H:%M:%S"),
            t.get("source", "?"),
            t.get("source_bpm", 0.0) or 0.0,
            t.get("source_beat", "-"),
            t.get("source_bar", "-"),
            t.get("source_32", "-"),
            s.get("link", "-"),
            s.get("beat_up", "-"), s.get("state_up", "-"),
            s.get("beat_frames", "-"),
            s.get("beats_elapsed", "-"),
            s.get("unit_software", ""),
        ))
        time.sleep(args.interval)
    print("%d requests" % n)


if __name__ == "__main__":
    main()

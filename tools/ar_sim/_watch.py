#!/usr/bin/env python3
"""WATCH THE BOARD'S TEMPO.

Polls /json/ar and prints the tempo row, so the board's rate, its score,
whether it thinks it is locked, and what the tempo tracker COST are all on
one line.

    python tools/ar_sim/_watch.py --samples 26 --interval 5

Do not poll it in a tight loop: the board's async_tcp task wedges after
about sixty to seventy cumulative GETs, so a fast poll can take the web
server down.  The default interval is five seconds.
"""

import argparse
import json
import time
import urllib.request

URL = "http://192.168.6.249/json/ar"


def fetch():
    with urllib.request.urlopen(URL, timeout=6) as r:
        return json.loads(r.read().decode("utf-8"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", type=int, default=26)
    ap.add_argument("--interval", type=float, default=5.0)
    args = ap.parse_args()

    # The board takes 40-60 s to answer after a flash.  Wait for it, but do
    # not hammer it: the async_tcp task wedges after about sixty to seventy
    # cumulative GETs.
    for i in range(20):
        try:
            fetch()
            print("board answered")
            break
        except Exception:                          # noqa: BLE001
            if i == 19:
                print("board did not answer in 100 s")
                return 1
            time.sleep(5.0)

    prev = None
    deltas = []
    for i in range(1, args.samples + 1):
        try:
            d = fetch()
        except Exception as exc:                      # noqa: BLE001
            print("  %2d: ERROR %s" % (i, exc))
            continue
        ar = d.get("AudioReactive", {})
        t = ar.get("tempo", {})
        tm = ar.get("timing_us", {})
        lv = ar.get("level", {})
        bpm = float(t.get("bpm", 0.0))
        if prev is not None and bpm > 0 and prev > 0:
            deltas.append(abs(bpm - prev))
        prev = bpm
        print("  %2d: bpm=%7.2f score=%.3f valid=%-5s beat=%s | "
              "tempo_us=%5d max=%5d | lvl=%.1f floor=%.1f"
              % (i, bpm, float(t.get("score", 0.0)), str(t.get("valid")),
                 t.get("beat_in_bar"), int(tm.get("tempo", 0)),
                 int(tm.get("tempo_max", 0)),
                 float(lv.get("level_db", 0.0)),
                 float(lv.get("noise_floor_db", 0.0))), flush=True)
        if i < args.samples:
            time.sleep(args.interval)

    if deltas:
        print("consecutive |delta|: max=%.2f  mean=%.3f  n=%d"
              % (max(deltas), sum(deltas) / len(deltas), len(deltas)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

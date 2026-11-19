"""
What does the board see when the platter is stopped?

The deck's beat clock keeps running when the music is stopped - the packets
keep coming and the beat numbers keep cycling - so the grid stays alive and
the sweep goes on.  Something has to say the music is not playing.

This polls the board's own numbers: the deck's position in the song, the
beat it is on, the grid's quality (how well the music explains the grid),
the level and the drift.

Usage:
    python tools/deck_stopped.py [seconds]
"""
import json
import sys
import time
import urllib.request

BOARD = "http://192.168.6.249"


def get(url):
    with urllib.request.urlopen(url, timeout=8) as r:
        return json.loads(r.read().decode("utf-8"))


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
    t0 = time.time()
    print("   t     position  beat  quality  level  drift  usable")
    while time.time() - t0 < seconds:
        try:
            ar = get(f"{BOARD}/json/ar")["AudioReactive"]
            t = ar["tempo"]
            lv = ar.get("level", {})
            print(f"  {time.time()-t0:5.2f}s  {t.get('source_32'):>7}  {t.get('source_beat'):>4}  "
                  f"{t.get('quality', 0):>7.3f}  {lv.get('level_db', 0):>6.1f}  "
                  f"{t.get('drift_ms', 0):>6.1f}  {t.get('usable')}")
        except Exception as e:
            print(f"  {time.time()-t0:5.2f}s  err {str(e)[:50]}")
        time.sleep(0.4)


if __name__ == "__main__":
    main()

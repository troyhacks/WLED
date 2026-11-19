"""
Watch the DECK's count of the song, and catch a track change.

The deck's beat count ("deck_elapsed") rides the status stream, which is
not the beat rate, so it arrives in steps.  This logs it against the
board's own beat counter so the two can be compared, and it prints the
moment the track id changes.

Usage:
    python tools/deck_songpos.py [polls] [seconds_between]
"""
import json
import sys
import time
import urllib.request

URL = "http://192.168.6.249/json/ar"


def get():
    with urllib.request.urlopen(URL, timeout=6) as r:
        return json.load(r)


def main():
    polls = int(sys.argv[1]) if len(sys.argv) > 1 else 30
    gap = float(sys.argv[2]) if len(sys.argv) > 2 else 1.5
    print("     t   deck_elapsed  deck_track  src_beat  our_count  dur_s")
    t0 = time.time()
    last_track = None
    for i in range(polls):
        try:
            d = get()
        except Exception as e:
            print("  poll %d failed: %s" % (i, e))
            time.sleep(gap)
            continue
        t = d.get("AudioReactive", {}).get("tempo", {})
        el = t.get("deck_elapsed", -1)
        tid = t.get("deck_track", -1)
        sb = t.get("source_beat", -1)
        bc = t.get("beat_count", -1)
        dur = t.get("deck_track_ms", -1)
        print("  %5.1f      %8d     %8d      %4d      %6d  %8.1f"
              % (time.time() - t0, el, tid, sb, bc, dur / 1000.0))
        if last_track is not None and tid != last_track:
            print("  *** THE TRACK CHANGED: %d -> %d, count now %d ***"
                  % (last_track, tid, el))
        last_track = tid
        time.sleep(gap)


if __name__ == "__main__":
    main()

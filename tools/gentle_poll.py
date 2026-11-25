#!/usr/bin/env python
# A GENTLE poll of the board.
#
# The board is a 360 MHz P4 with two cores and under 512 KB of internal RAM
# doing a lot, so this asks for as little as possible:
#
#   /json/info  - small (3 KB), and it carries the strip's measured fps
#   /json/ar    - big (17 KB), only every AR_EVERY seconds, and it carries
#                 the tempo, the publish lock's counters and the control
#                 surface's counters
#
# The cadence is deliberately slow: the point is to watch, not to load.
import json, urllib.request, time, sys

HOST = "http://192.168.6.249"
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 600.0
INFO_EVERY = 10.0
AR_EVERY = 30.0


def get(path):
    t0 = time.time()
    with urllib.request.urlopen(HOST + path, timeout=25) as f:
        body = f.read()
    return time.time() - t0, time.time(), json.loads(body)


t_start = time.time()
next_info = 0.0
next_ar = 0.0
prev_ar = None
prev_info = None
while time.time() - t_start < DURATION:
    now = time.time() - t_start
    if now >= next_info:
        next_info = now + INFO_EVERY
        try:
            lat, t, info = get("/json/info")
            fps = info["leds"]["fps"]
            free = info.get("freeheap")
            psram = info.get("psram")
            line = "t=%6.1f  fps=%3s  heap=%s  psram_free=%s" % (now, fps, free, psram)
            if prev_info is not None:
                line += "  (%.1fs since the last)"
            print(line, flush=True)
            prev_info = t
        except Exception as e:
            print("t=%6.1f  info ERROR %r" % (now, e), flush=True)
    if now >= next_ar:
        next_ar = now + AR_EVERY
        try:
            lat, t, ar = get("/json/ar")
            A = ar["AudioReactive"]
            h = ar.get("HIDCTL", {})
            tm = A["tempo"]
            line = ("t=%6.1f  bpm=%7.2f score=%.2f beat=%d  analysis=%d frames  "
                    "pub_w=%s pub_s=%s  F1 out=%s in=%s stuck=%s busy=%s  lat=%.2fs"
                    % (now, tm["bpm"], tm["score"], tm["beat_in_bar"],
                       A["timing_us"]["frames_total"], tm.get("pub_waits"),
                       tm.get("pub_spins"), h.get("out_sent"), h.get("reports_in"),
                       h.get("out_stuck"), h.get("out_busy"), lat))
            print(line, flush=True)
            prev_ar = t
        except Exception as e:
            print("t=%6.1f  ar ERROR %r" % (now, e), flush=True)
    time.sleep(1.0)
print("done", flush=True)

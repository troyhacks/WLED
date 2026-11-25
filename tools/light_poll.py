#!/usr/bin/env python
# A light poll: /json/info only (small, but it still takes the shared JSON
# document, so it exercises the fix), every INTERVAL seconds.
import json, urllib.request, time, sys

HOST = "http://192.168.6.249"
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 600.0
INTERVAL = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
WITH_AR = (len(sys.argv) > 3 and sys.argv[3] == "ar")

t_start = time.time()
while time.time() - t_start < DURATION:
    try:
        t0 = time.time()
        with urllib.request.urlopen(HOST + "/json/info", timeout=20) as f:
            info = json.load(f)
        t1 = time.time()
        extra = ""
        if WITH_AR:
            with urllib.request.urlopen(HOST + "/json/ar", timeout=25) as f:
                ar = json.load(f)
            A = ar["AudioReactive"]
            extra = "  bpm=%7.2f score=%.2f  analysis_frames=%d" % (
                A["tempo"]["bpm"], A["tempo"]["score"], A["timing_us"]["frames_total"])
        print("t=%6.1f  fps=%3s  ws=%s  heap=%s  psram_used=%s/%s  lat=%.2fs%s" % (
            time.time() - t_start, info["leds"]["fps"], info.get("ws"),
            info.get("freeheap"), info.get("psusedram"), info.get("psram"),
            t1 - t0, extra), flush=True)
    except Exception as e:
        print("t=%6.1f  ERROR %r" % (time.time() - t_start, e), flush=True)
    time.sleep(INTERVAL)

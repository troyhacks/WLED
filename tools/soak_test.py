#!/usr/bin/env python
# A soak test for the board: poll /json/ar on a fixed cadence (the thing
# that used to take the strip from 44 fps to 4 fps) and watch everything
# that has to stay healthy while it happens.
#
#   - the AR's analysis rate          (want 93.75 fps)
#   - the UDP sync rate               (the Background loop's rate)
#   - the LED strip's measured fps    (want ~44)
#   - the F1's reports in and out     (want both climbing)
#   - the tempo's rate and its score  (the tracker)
#   - the server's first-byte times   (the shared JSON document)
import json, urllib.request, time, sys

HOST = "http://192.168.6.249"
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 600.0
INTERVAL = 5.0


def get(path):
    t0 = time.time()
    with urllib.request.urlopen(HOST + path, timeout=25) as f:
        body = f.read()
    t1 = time.time()
    return t1 - t0, t1, json.loads(body)


rows = []
t_start = time.time()
prev = None
while time.time() - t_start < DURATION:
    try:
        lat_ar, t_ar, ar = get("/json/ar")
        lat_info, t_info, info = get("/json/info")
        A = ar["AudioReactive"]
        h = ar.get("HIDCTL", {})
        t = A["tempo"]
        now = t_ar
        row = {
            "t": now - t_start,
            "lat_ar": lat_ar,
            "lat_info": lat_info,
            "frames": A["timing_us"]["frames_total"],
            "tx": A["udp_sync"]["tx_calls"],
            "beat": t["beat_count"],
            "fps": info["leds"]["fps"],
            "out_sent": h.get("out_sent"),
            "in": h.get("reports_in"),
            "stuck": h.get("out_stuck"),
            "busy": h.get("out_busy"),
            "bpm": t["bpm"],
            "score": t["score"],
            "qual": t["quality"],
            "ws": info.get("ws"),
            "pub_waits": t.get("pub_waits"),
            "pub_spins": t.get("pub_spins"),
            "heap": info.get("freeheap"),
        }
        if prev:
            dt = now - prev["_t"]
            row["analysis"] = (row["frames"] - prev["frames"]) / dt
            row["udp"] = (row["tx"] - prev["tx"]) / dt
            row["beat_s"] = (row["beat"] - prev["beat"]) / dt
        row["_t"] = now
        rows.append(row)
        prev = row
        if "analysis" in row:
            print(
                "t=%6.1f  fps=%3s  analysis=%5.1f  udp=%6.1f/s  beat=%5.2f/s  "
                "bpm=%7.2f score=%.2f  F1 out=%s in=%s stuck=%s busy=%s  "
                "pub_w=%s pub_s=%s lat_ar=%.2fs lat_info=%.2fs heap=%s"
                % (
                    row["t"],
                    row["fps"],
                    row["analysis"],
                    row["udp"],
                    row["beat_s"],
                    row["bpm"],
                    row["score"],
                    row["out_sent"],
                    row["in"],
                    row["stuck"],
                    row["busy"],
                    row["pub_waits"],
                    row["pub_spins"],
                    row["lat_ar"],
                    row["lat_info"],
                    row["heap"],
                ),
                flush=True,
            )
    except Exception as e:
        print("t=%6.1f  ERROR %r" % (time.time() - t_start, e), flush=True)
    time.sleep(INTERVAL)

if rows:
    keys = [k for k in rows[0].keys() if k != "_t"]
    with open("soak.csv", "w", encoding="utf-8") as f:
        f.write(",".join(keys) + "\n")
        for r in rows:
            f.write(",".join(str(r.get(k, "")) for k in keys) + "\n")
    print("wrote soak.csv")

#!/usr/bin/env python
# Poll the board while the console capture runs, and record the rates that
# show how healthy the core-0 loops are.
#
# The UDP sync is sent from the AudioReactive usermod's loop() - the
# Background task - so the UDP datagram rate IS that loop's rate (two
# datagrams per analysis frame).  The analysis rate is the AudioReactive
# task's.  The first-byte time of /json/ar is the core-0 server's latency.
import json, urllib.request, time, sys

HOST = "http://192.168.6.249"
OUT = sys.argv[1] if len(sys.argv) > 1 else "rate_poll.csv"
DURATION = float(sys.argv[2]) if len(sys.argv) > 2 else 330.0
INTERVAL = 5.0

rows = []
t_start = time.time()
prev = None
while time.time() - t_start < DURATION:
    try:
        t0 = time.time()
        with urllib.request.urlopen(HOST + "/json/ar", timeout=20) as f:
            d = json.load(f)
        t1 = time.time()
        A = d["AudioReactive"]
        h = d["HIDCTL"]
        now = time.time()
        row = {
            "t": now - t_start,
            "ar_latency": t1 - t0,
            "frames": A["timing_us"]["frames_total"],
            "tx": A["udp_sync"]["tx_calls"],
            "beat": A["tempo"]["beat_count"],
            "bpm": A["tempo"]["bpm"],
            "score": A["tempo"]["score"],
            "out_sent": h.get("out_sent"),
            "reports_in": h.get("reports_in"),
            "out_stuck": h.get("out_stuck"),
            "out_busy": h.get("out_busy"),
        }
        if prev:
            dt = now - prev["_t"]
            row["analysis_fps"] = (row["frames"] - prev["frames"]) / dt
            row["udp_per_s"] = (row["tx"] - prev["tx"]) / dt
            row["beat_per_s"] = (row["beat"] - prev["beat"]) / dt
        row["_t"] = now
        rows.append(row)
        prev = row
        if "analysis_fps" in row:
            print(
                "t=%6.1f  ar_latency=%5.3fs  analysis=%6.1f fps  udp=%6.1f/s  "
                "beat=%5.2f/s  bpm=%7.2f  score=%.2f  out_sent=%s in=%s"
                % (
                    row["t"],
                    row["ar_latency"],
                    row["analysis_fps"],
                    row["udp_per_s"],
                    row["beat_per_s"],
                    row["bpm"],
                    row["score"],
                    row["out_sent"],
                    row["reports_in"],
                ),
                flush=True,
            )
    except Exception as e:
        print("t=%6.1f  ERROR %r" % (time.time() - t_start, e), flush=True)
    time.sleep(INTERVAL)

with open(OUT, "w", encoding="utf-8") as f:
    if rows:
        f.write(",".join(rows[0].keys()) + "\n")
        for r in rows:
            f.write(",".join(str(r.get(k, "")) for k in rows[0].keys()) + "\n")
print("wrote", OUT)

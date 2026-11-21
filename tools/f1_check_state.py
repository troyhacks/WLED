import json, time, urllib.request
URL = "http://192.168.6.249/json/ar"
deadline = time.time() + 150
while time.time() < deadline:
    try:
        with urllib.request.urlopen(URL, timeout=4) as r:
            d = json.load(r)
        h = d.get("HIDCTL")
        if h:
            for k in ("connected","device","page","page_count","active_preset","wheel","wheel_seen",
                      "presets_cached","music_slot","report_status","reports_ok","demo"):
                print(f"  {k:16} {h.get(k)}")
            break
    except Exception as e:
        last = e
    time.sleep(5)
else:
    print("board did not answer:", last)

import json, time, urllib.request
URL = "http://192.168.6.249/json/ar"
deadline = time.time() + 150
h = None
while time.time() < deadline:
    try:
        with urllib.request.urlopen(URL, timeout=4) as r:
            h = json.load(r).get("HIDCTL")
        if h: break
    except Exception:
        pass
    time.sleep(5)
if not h:
    raise SystemExit("board did not answer")
for k in ("connected","device","page","page_count","active_preset","wheel","wheel_seen",
          "presets_cached","music_slot","demo"):
    print(f"  {k:16} {h.get(k)}")
raw = bytes.fromhex(h.get("led_report",""))
if raw:
    ap, page = h.get("active_preset",0), h.get("page",1)
    off = (page-1)*16
    print(f"-- page {page} covers presets {off+1}-{off+16}, active {ap}")
    for k in range(16):
        b,r,g = raw[25+k*3], raw[25+k*3+1], raw[25+k*3+2]
        nm = {(0,0,0x7f):"GREEN",(0,0x7f,0):"red",(0x7f,0,0):"blue",
              (0x7f,0x7f,0):"magenta",(0,0x7f,0x7f):"yellow"}.get((b,r,g))
        if nm: print(f"   pad{k+1:2} slot{off+k+1:4}  {nm}")
    print("   bars:", [hex(raw[73+i*2]) for i in range(4)])
    print("   display:", " ".join(f"{x:02X}" for x in raw[1:17]))

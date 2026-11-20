"""Watch the board's HIDCTL state: buttons and control values, with changes."""
import json
import time
import urllib.request

BASE = "http://192.168.6.249"


def get(p):
    with urllib.request.urlopen(f"{BASE}{p}", timeout=15) as r:
        return json.loads(r.read().decode())


st = get("/json/state")
print("state keys:", ", ".join(st.keys()))
print({k: st[k] for k in list(st.keys())[:12]})
print("seg:", json.dumps(st.get("seg"))[:260])
print()

last_b = None
last_v = None
for i in range(40):
    try:
        h = get("/json/ar").get("HIDCTL", {})
    except Exception as e:
        print("http error:", e)
        time.sleep(2)
        continue
    b = h.get("buttons")
    v = h.get("values")
    bits = [g for g in range(32) if b and (b >> g) & 1]
    if b != last_b or v != last_v:
        print(f"buttons={b} bits={bits}")
        print(f"  values={v}")
        last_b, last_v = b, v
    time.sleep(1)

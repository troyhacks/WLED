"""Print the board's HIDCTL state (from /json/ar) and the current WLED state."""
import json
import urllib.request

BASE = "http://192.168.6.249"


def get(p):
    with urllib.request.urlopen(f"{BASE}{p}", timeout=15) as r:
        return json.loads(r.read().decode())


ar = get("/json/ar")
print("HIDCTL:", json.dumps(ar.get("HIDCTL"))[:260])
st = get("/json/state")
seg = (st.get("seg") or [{}])[0]
print(f"on={st.get('on')} bri={st.get('bri')} ps={st.get('ps')} "
      f"seg0 fx={seg.get('fx')} pal={seg.get('pal')} bri={seg.get('bri')}")

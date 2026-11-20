"""List the board's saved presets with their names."""
import json
import urllib.request

with urllib.request.urlopen("http://192.168.6.249/presets.json", timeout=20) as r:
    d = json.loads(r.read().decode())

for k in sorted(d, key=lambda x: int(x)):
    v = d[k]
    if not isinstance(v, dict):
        print(f"slot {k:>3}: (not a dict)")
        continue
    nm = v.get("n", None)
    segs = v.get("seg") or []
    print(f"slot {k:>3}: name={nm!r} segs={len(segs)}")

"""Read the LED report the board last sent, and decode the pad bytes."""
import json
import urllib.request

with urllib.request.urlopen("http://192.168.6.249/json/ar", timeout=20) as r:
    h = json.loads(r.read().decode()).get("HIDCTL", {})

rep = h.get("led_report", "")
print("connected:", h.get("connected"), " report bytes:", len(rep) // 2)
if not rep:
    raise SystemExit("no report yet")

b = bytes.fromhex(rep)
print("byte 0 (report ID):", hex(b[0]))

print("\ndisplay (bytes 1..16):")
print("  right digit (1..8):", " ".join(f"{x:02X}" for x in b[1:9]))
print("  left  digit (9..16):", " ".join(f"{x:02X}" for x in b[9:17]))

print("\ntransport buttons (17..24):")
for i, x in enumerate(b[17:25]):
    print(f"  byte {17+i}: {x:02X}", end="")
print()

print("\npads (bytes 25..72, blue, red, green):")
for p in range(16):
    base = 25 + p * 3
    bb, r, g = b[base], b[base + 1], b[base + 2]
    name = []
    if bb:
        name.append("BLUE")
    if r:
        name.append("RED")
    if g:
        name.append("GREEN")
    print(f"  pad{p+1:>2}: bytes {base}..{base+2} = {bb:02X} {r:02X} {g:02X}  {' '.join(name) or '(dark)'}")

print("\nlower buttons (73..80):")
print("  ", " ".join(f"{x:02X}" for x in b[73:81]))

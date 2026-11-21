"""What does report byte 5 actually track?

The NexAdn notes say byte 5 is the selector-wheel's position, but the board
saw it never move. This walks a capture and prints every change of byte 5
along with what else changed at the same moment.
"""
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "tools/f1_reports_final.log"
prev = None
prev5 = None
count = 0
for line in open(path, encoding="utf-8"):
    parts = line.split()
    if len(parts) < 2:
        continue
    try:
        t = float(parts[0])
        d = bytes(int(b, 16) for b in parts[1:])
    except ValueError:
        continue
    if len(d) < 22:
        continue
    if prev is not None and d[5] != prev5:
        changed = [i for i in range(1, 5) if d[i] != prev[i]]
        print(f"{t:8.3f}s  byte5 {prev5:3d} -> {d[5]:3d}   "
              f"buttons also changed: {changed}  raw={d[:6].hex(' ')}")
        count += 1
    prev = d
    prev5 = d[5]
print(f"\n{count} changes of byte 5")

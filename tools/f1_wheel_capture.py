"""Capture the F1's input reports and watch the wheel's position byte.

Byte 5 of the 22-byte report is the encoder's position: it counts up one
step per detent clockwise and down anticlockwise, wrapping at 8 bits.

Usage:
    python tools/f1_wheel_capture.py [seconds]
"""
import sys
import time

import hid

SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0

dev = hid.device()
for d in hid.enumerate():
    if d["vendor_id"] == 0x17CC and d["product_id"] == 0x1120:
        dev.open_path(d["path"])
        print("opened:", d["product_string"], flush=True)
        break
else:
    sys.exit("F1 not found - is it plugged into this PC?")

print(f"capturing {SECONDS:.0f}s - turn the round encoder", flush=True)
prev = None
t0 = time.time()
moved = 0
while time.time() - t0 < SECONDS:
    data = dev.read(64, 200)
    if not data:
        continue
    b = bytes(data)
    if len(b) < 6:
        continue
    if prev is not None and b[5] != prev:
        moved += 1
        print(f"{time.time()-t0:7.2f}s  byte5 {prev:3d} -> {b[5]:3d}   {b[:8].hex(' ')}",
              flush=True)
    prev = b[5]

print(f"\n{moved} wheel moves seen", flush=True)

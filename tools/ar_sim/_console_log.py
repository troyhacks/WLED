#!/usr/bin/env python3
"""Open COM4 and capture whatever the board says.

Opening the port resets the P4, so this is also a way out of a wedged
board.  The boot log is lossy on this board (the USB CDC drops most of the
self-test's spew), so the interesting lines are the crashes.
"""

import sys
import time

import serial

DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 70.0
OUT = sys.argv[2] if len(sys.argv) > 2 else "console.log"

port = serial.Serial("COM4", 115200, timeout=0.2)
buf = bytearray()
t0 = time.time()
while time.time() - t0 < DURATION:
    try:
        chunk = port.read(4096)
    except Exception:
        continue
    if chunk:
        buf.extend(chunk)
    with open(OUT, "ab") as f:
        f.write(chunk)
port.close()
text = buf.decode("utf-8", "replace")
print("captured %d bytes" % len(buf))
keys = ("panic", "Backtrace", "Abort", "assert", "Guru", "Fatal", "ERROR",
        "reboot", "rst:", "Booted", "wled", "ProLink", "tempo")
for line in text.splitlines():
    if any(k in line for k in keys):
        print(line[:200])

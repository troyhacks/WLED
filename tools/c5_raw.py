"""Dump the ESP32-C5's serial output raw, to a file.

Line-based reading loses whatever does not end in a newline, and a board
that crashes mid-line says the most in exactly that part.  This reads
whatever arrives and writes it out as it comes.

Usage:  python tools/c5_raw.py [seconds] [port] [outfile]
"""

import sys
import time

try:
    import serial
except ImportError:
    print("pyserial is not installed")
    sys.exit(1)

seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0
port = sys.argv[2] if len(sys.argv) > 2 else "COM10"
out = sys.argv[3] if len(sys.argv) > 3 else "c5_raw.log"

ser = serial.Serial(port, 115200, timeout=0.1)
# RTS high pulls EN low on the ESP32-C5: that is a reset.
ser.dtr = False
ser.rts = True
time.sleep(0.2)
ser.rts = False
time.sleep(0.3)

with open(out, "wb") as f:
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            data = ser.read(4096)
        except Exception as exc:      # noqa: BLE001
            f.write(("read error: %s\n" % exc).encode())
            break
        if data:
            f.write(data)
            f.flush()

ser.close()
print("wrote", out)

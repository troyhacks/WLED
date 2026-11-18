"""Read the ESP32-C5's console for a while and print it.

The C5's console is the USB CDC, so this opens COM10 and reads it.  It
also nudges the board with DTR/RTS so that a board that is sitting in the
bootloader starts running.

Usage:  python tools/c5_monitor.py [seconds] [port]
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

ser = serial.Serial(port, 115200, timeout=0.2)
# On the ESP32-C5 the RTS/DTR pair is inverted: RTS high pulls EN low, which
# is a reset.  Opening the port asserts both, so the chip is held in reset
# until RTS is let go again - which is exactly the reset we want.
ser.dtr = False
ser.rts = True
time.sleep(0.2)
ser.rts = False
time.sleep(0.3)

deadline = time.time() + seconds
while time.time() < deadline:
    try:
        line = ser.readline()
    except Exception as exc:      # noqa: BLE001
        print("read error:", exc)
        break
    if line:
        try:
            print(line.decode("utf-8", "replace").rstrip())
        except Exception:           # noqa: BLE001
            print(repr(line))

ser.close()

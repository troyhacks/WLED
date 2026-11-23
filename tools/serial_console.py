#!/usr/bin/env python
# Read a board's serial console, optionally send something first, and print it.
#
#   python tools/serial_console.py COM11 25            # just listen
#   python tools/serial_console.py COM11 25 "R"         # send R, then listen
#
# The P4's USB serial port resets the chip when DTR falls, so DTR is left
# low here (the same thing the pio monitor does with monitor_dtr = 0).

import sys
import time
import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
send = sys.argv[3] if len(sys.argv) > 3 else ""

ser = serial.Serial(port, 115200, timeout=0.2, dsrdtr=False, rtscts=False)
# dtr = False and rts = False: no edge on either line.
ser.dtr = False
ser.rts = False

if send:
    time.sleep(0.5)
    ser.write(send.encode())
    ser.flush()

end = time.time() + seconds
buf = bytearray()
while time.time() < end:
    try:
        data = ser.read(4096)
    except Exception:
        break
    if data:
        buf += data
ser.close()
sys.stdout.write(buf.decode("utf-8", "replace"))

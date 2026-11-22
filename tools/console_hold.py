"""
Hold COM4 open and log the console, without ever resetting the board.

The P4's USB-Serial-JTAG resets the chip on a DTR FALLING EDGE, and Windows
drops DTR when the port is closed.  So the port is opened with DTR already
low (pyserial's do_not_open, then dtr = False, then open): there is no
rising edge and no falling edge, and closing the port at the end is
harmless.

Usage:
    python tools/console_hold.py <outfile> [seconds]
"""
import sys
import time

import serial

OUT = sys.argv[1] if len(sys.argv) > 1 else "build_output/console.log"
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 600.0

ser = serial.Serial()
ser.port = "COM4"
ser.baudrate = 115200
ser.timeout = 0.2
ser.dsrdtr = False
ser.rtscts = False
ser.dtr = False          # before open: the port comes up with DTR low
ser.rts = False
ser.open()

f = open(OUT, "w", encoding="utf-8", errors="replace")
t0 = time.time()
try:
    while time.time() - t0 < SECONDS:
        try:
            line = ser.readline()
        except serial.SerialTimeoutException:
            continue
        except serial.SerialException as e:
            print("serial error: %s" % e)
            break
        if not line:
            continue
        text = line.decode("utf-8", errors="replace").rstrip()
        f.write(text + "\n")
        f.flush()
        print("%7.1f  %s" % (time.time() - t0, text), flush=True)
finally:
    f.close()
    try:
        ser.close()
    except Exception:
        pass

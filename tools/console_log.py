"""
Log the board's serial console to a file, and print the new lines.

Opening COM4 RESETS the board, so the boot log comes first and the
interesting lines come after it.  The log is written as it goes, so a
long capture can be stopped at any time and still be read.

Usage:
    python tools/console_log.py <outfile> [seconds]
"""
import sys
import time

import serial

OUT = sys.argv[1] if len(sys.argv) > 1 else "build_output/console.log"
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 300.0

ser = serial.Serial("COM4", 115200, timeout=0.2, dsrdtr=False, rtscts=False)
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

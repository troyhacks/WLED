"""
Capture the board's serial console FROM THE BOOT.

tools/console_log.py opens COM4, which asserts DTR and therefore does not
reset the board - it captures whatever is being printed now, and the boot
lines are long gone. This one resets the board with the port already open,
so the whole boot is captured.

Why the port has to be open first: the P4 resets on a DTR FALLING edge, and
Windows drops DTR when the port is closed. So the port is opened with DTR
already low (pyserial's do_not_open, then dtr = False, then open) - there is
no edge at all - and the reset is made afterwards by pulsing DTR from low to
high and back down.

Usage:
    python tools/console_boot.py <outfile> [seconds]
"""
import sys
import time

import serial

OUT = sys.argv[1] if len(sys.argv) > 1 else "build_output/boot.log"
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 40.0

ser = serial.Serial()
ser.port = "COM4"
ser.baudrate = 115200
ser.timeout = 0.2
ser.dsrdtr = False
ser.rtscts = False
ser.dtr = False          # before open: the port comes up with DTR low
ser.rts = False
ser.open()

# Now reset it: a rising edge on DTR, then a falling edge - the fall is what
# the P4 watches.
try:
    ser.dtr = True
    time.sleep(0.1)
    ser.dtr = False
except Exception as e:
    print("could not reset the board: %s" % e)

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

"""Read the ESP32-C5's console for a while and print it.

The C5's console is the USB CDC, so this opens COM10 and reads it.

CAREFUL: the board's USB serial/JTAG watches DTR and RTS, and it is
reset by them.  Opening the port asserts both, which the board takes in
stride, but CLOSING it lets them go and that is a reset.  A board that is
reset takes a second to find the network and everything the user asked
of it stops until it does - so this script says so, and the console is
read from the network instead when the board must not be disturbed.

The lines are left where pyserial puts them (both asserted).  Setting
them the other way is itself a reset.

Usage:  python tools/c5_monitor.py [seconds] [port] [--reset]
"""

import sys
import time

try:
    import serial
except ImportError:
    print("pyserial is not installed")
    sys.exit(1)

args = [a for a in sys.argv[1:] if a != "--reset"]
do_reset = "--reset" in sys.argv[1:]

seconds = float(args[0]) if len(args) > 0 else 12.0
port = args[1] if len(args) > 1 else "COM10"

ser = serial.Serial(port, 115200, timeout=0.2)

if do_reset:
    # The board is in the bootloader and has to be started.  The lines are
    # pulsed: a change on either of them is a reset on this board.
    ser.dtr = False
    ser.rts = False
    time.sleep(0.2)
    ser.dtr = True
    ser.rts = True
    time.sleep(0.1)
    ser.dtr = False
    ser.rts = False
    time.sleep(0.5)

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

print("closing the port resets the board - it comes back in a few seconds")
ser.close()

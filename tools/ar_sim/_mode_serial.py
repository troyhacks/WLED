#!/usr/bin/env python3
"""The serial keys for the mode: 'M' is DJ SET, 'P' is PLAYLIST.

Opening COM4 RESETS the P4 whatever DTR does, so the port is opened, the
board is given time to boot (about a minute), and only then are the keys
sent - anything sent earlier is swallowed by the boot log.
"""
import sys
import time

import serial

PORT = "COM4"
BOOT_S = 60


def main():
    ser = serial.Serial(PORT, 115200, timeout=0.5)
    print("opened %s - the board is resetting; waiting %d s" % (PORT, BOOT_S),
          flush=True)
    time.sleep(BOOT_S)
    # Drain whatever the boot left in the buffer.
    t0 = time.time()
    while time.time() - t0 < 3:
        try:
            if ser.in_waiting:
                ser.read(ser.in_waiting)
            else:
                time.sleep(0.2)
        except Exception:
            break
    for key in ("M", "P"):
        ser.write(key.encode())
        ser.flush()
        print("sent %r" % key, flush=True)
        time.sleep(2.0)
        out = b""
        t0 = time.time()
        while time.time() - t0 < 2.0:
            try:
                if ser.in_waiting:
                    out += ser.read(ser.in_waiting)
                else:
                    time.sleep(0.1)
            except Exception:
                break
        print("   said: %s" % out.decode("utf-8", "replace").strip(), flush=True)
    ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python
# Open the P4's console on COM4, reboot it, and capture everything with
# timestamps for five minutes.
#
# The console is the USB-Serial-JTAG port.  DTR and RTS are held LOW for
# the whole session: a DTR FALL resets the P4 (the board is on the JTAG
# UART, and Windows drops DTR when the port closes).
import serial, time, sys, os

PORT = "COM4"
BAUD = 115200
OUT = sys.argv[1] if len(sys.argv) > 1 else "console_capture.log"
DURATION = float(sys.argv[2]) if len(sys.argv) > 2 else 330.0
REBOOT = (len(sys.argv) <= 3 or sys.argv[3] != "noreboot")
TASK_EVERY = 60.0

ser = serial.Serial(PORT, BAUD, timeout=0.2)
ser.dtr = False
ser.rts = False

t0 = time.time()
log = open(OUT, "w", encoding="utf-8", errors="replace")


def stamp():
    return time.time() - t0


def emit(tag, data):
    line = "[%8.3f] %s %s" % (stamp(), tag, data.rstrip("\r\n"))
    log.write(line + "\n")
    log.flush()


# The reboot.  'R' is WLEDMM's force-reboot.
time.sleep(0.5)
if REBOOT:
    ser.write(b"R")
    emit(">>", "sent R (reboot)")

# 'C' gives the task list.
next_c = 30.0
buf = b""
while stamp() < DURATION:
    try:
        chunk = ser.read(4096)
    except Exception as e:
        emit("!!", "read error: %r" % (e,))
        break
    if chunk:
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            emit("<<", line.decode("utf-8", "replace"))
    if stamp() >= next_c:
        next_c += TASK_EVERY
        ser.write(b"C")
        emit(">>", "sent C (task list)")

log.write("---- capture ended at %.1f s ----\n" % stamp())
log.close()
# Leave DTR asserted across the close: the P4 resets on a DTR CHANGE, and
# the driver's close would otherwise drop it.  If the board still reboots
# when the port closes, that is what happened.
ser.dtr = True
time.sleep(0.2)
ser.close()
print("done, %d lines" % sum(1 for _ in open(OUT, encoding="utf-8", errors="replace")))

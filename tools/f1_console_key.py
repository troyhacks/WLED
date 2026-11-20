"""Send a key to the P4's serial console and show what the board printed.

Opening COM4 resets the P4, so the boot has to finish before the key
lands -- anything sent earlier is swallowed by the boot log.

Usage:
    python tools/f1_console_key.py r
"""
import json
import sys
import time
import urllib.request

import serial

KEY = sys.argv[1] if len(sys.argv) > 1 else "r"
BOOT_WAIT = 65.0


def demo_state():
    with urllib.request.urlopen("http://192.168.6.249/json/ar", timeout=20) as r:
        h = json.loads(r.read().decode()).get("HIDCTL", {})
    return h.get("demo")


ser = serial.Serial("COM4", 115200, timeout=0.2, dsrdtr=False, rtscts=False)
print(f"opened COM4 (the board resets); waiting {BOOT_WAIT:.0f}s for the boot", flush=True)

# Drain the boot log while we wait, and keep the last lines.
tail = []
t0 = time.time()
while time.time() - t0 < BOOT_WAIT:
    try:
        line = ser.readline()
    except Exception:
        continue
    if line:
        text = line.decode("utf-8", errors="replace").rstrip()
        tail.append(text)
        tail = tail[-25:]

print("--- last lines of the boot log ---", flush=True)
for t in tail:
    print("   " + t, flush=True)

print(f"\ndemo before: {demo_state()}", flush=True)
ser.write(KEY.encode())
ser.flush()
print(f"sent '{KEY}'", flush=True)

time.sleep(3)
print("--- what the console said ---", flush=True)
t0 = time.time()
while time.time() - t0 < 3:
    try:
        line = ser.readline()
    except Exception:
        continue
    if line:
        print("   " + line.decode("utf-8", errors="replace").rstrip(), flush=True)

print(f"demo after : {demo_state()}", flush=True)
ser.close()

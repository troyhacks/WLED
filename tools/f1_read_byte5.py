"""Read the F1's encoder byte on the PC.

Opens the F1 with hidapi and prints byte 5 of every report - the encoder's
position - so the value can be compared with what the P4 sees at the same
moment. With no software running against the device, the encoder must
still report.

    python tools/f1_read_byte5.py [seconds]
"""
import sys, time
import hid

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 6.0
dev = None
for d in hid.enumerate(0x17CC, 0x1120):
    dev = d
    break
if dev is None:
    raise SystemExit("the F1 is not on this machine")

h = hid.device()
h.open_path(dev.path)
print(f"opened {dev.path}  product={dev.product_string!r}")
t0 = time.time()
last = None
while time.time() - t0 < secs:
    try:
        d = h.read(22, timeout_ms=200)
    except Exception:
        continue
    if not d:
        continue
    b5 = d[5] if len(d) > 5 else None
    if b5 != last:
        print(f"  t={time.time()-t0:5.2f}s  len={len(d)}  byte5={b5}  "
              f"full={' '.join(f'{x:02X}' for x in d[:8])}")
        last = b5
h.close()

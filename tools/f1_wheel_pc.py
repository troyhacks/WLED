"""Watch the F1's encoder on the PC: byte 5, and the report rate.

The F1 must be plugged into the PC (not the board) for this. It prints
every change of byte 5 - the encoder's position counter - and the rate at
which reports arrive, because the F1 speeds its reports up while the wheel
is moving. Turn the wheel while it runs.

    python tools/f1_wheel_pc.py [seconds]
"""
import sys, time
import hid

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
dev = None
for d in hid.enumerate(0x17CC, 0x1120):
    dev = d
    break
if dev is None:
    raise SystemExit("the F1 is not on this machine (plug it into the PC)")

h = hid.device()
try:
    h.open_path(dev.path)
except Exception as e:
    raise SystemExit(f"cannot open the F1: {e}\n"
                     "close Traktor / the NI driver and try again")
print(f"opened {dev.path}  product={dev.product_string!r}")

t0 = time.time()
last = None
count = 0
rate_mark = t0
rate_count = 0
while time.time() - t0 < secs:
    try:
        d = h.read(22, timeout_ms=200)
    except Exception:
        continue
    if not d:
        continue
    count += 1
    rate_count += 1
    b5 = d[5] if len(d) > 5 else None
    if b5 != last:
        delta = None if last is None else ((b5 - last + 128) % 256) - 128
        print(f"  t={time.time() - t0:6.2f}s  byte5={b5}  delta={delta}  "
              f"report={' '.join(f'{x:02X}' for x in d)}", flush=True)
        last = b5
    now = time.time()
    if now - rate_mark >= 2.0:
        print(f"  t={now - t0:6.2f}s  rate={rate_count / (now - rate_mark):6.1f} Hz",
              flush=True)
        rate_mark = now
        rate_count = 0
h.close()
print(f"{count} reports in {secs:.0f}s")

"""The encoder test: does the F1 report its wheel while we are painting?

The one difference left between a PC's HID stack and ours: a PC never
writes to the controller, we send the 81-byte LED report ten times a
second. If the F1 re-initialises its input state on an output report,
the encoder's position is back to zero every time we paint.

So: turn the painting off, turn the wheel, see if byte 5 moves. Then
turn the painting back on and turn it again.
"""
import json, time, urllib.request

URL = "http://192.168.6.249/json/ar"
STATE = "http://192.168.6.249/json/state"

def ar():
    return json.load(urllib.request.urlopen(URL, timeout=5)).get('HIDCTL', {})

def post(body):
    req = urllib.request.Request(STATE, data=json.dumps(body).encode(),
                                 headers={'Content-Type': 'application/json'})
    urllib.request.urlopen(req, timeout=6).read()

print("waiting for the F1 to appear on the board ...", flush=True)
deadline = time.time() + 900
while time.time() < deadline:
    try:
        h = ar()
        if h.get('connected'):
            break
    except Exception:
        pass
    time.sleep(4)
else:
    raise SystemExit("the F1 never showed up")

print(f"connected.  class_status={h.get('class_status')} class_data={h.get('class_data')}",
      flush=True)

for paint in (False, True):
    post({"HIDCTL": {"paint": paint}})
    time.sleep(1.0)
    h = ar()
    print(f"\n=== painting {'ON' if paint else 'OFF'} "
          f"(paint flag={h.get('demo')}) ===", flush=True)
    print(">>> TURN THE ENCODER NOW, for about 20 seconds <<<\n", flush=True)
    base_min, base_max = h.get('wheel_min', 0), h.get('wheel_max', 0)
    t0 = time.time()
    while time.time() - t0 < 30:
        time.sleep(3)
        h = ar()
        print(f"  t={time.time()-t0:4.0f}s wheel={h.get('wheel')} "
              f"min={h.get('wheel_min')} max={h.get('wheel_max')} "
              f"moved={h.get('wheel_moved')} reports={h.get('reports_in')}", flush=True)
    print(f"  --> with painting {'ON' if paint else 'OFF'}: "
          f"min {base_min}->{h.get('wheel_min')}, "
          f"max {base_max}->{h.get('wheel_max')}, "
          f"moved={h.get('wheel_moved')}", flush=True)

post({"HIDCTL": {"paint": True}})
print("\ndone - painting left on", flush=True)

"""Watch the board for the F1's encoder, byte by byte.

Polls /json/ar and prints every report that the board saw CHANGE, with the
bytes that moved, plus the wheel counters. This is the live test for the
rotary encoder: turn the wheel while it runs.

    python tools/f1_wheel_live.py [seconds]
"""
import json, sys, time, urllib.request

HOST = 'http://192.168.6.131'
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 300.0


def hid():
    for _ in range(3):
        try:
            with urllib.request.urlopen(f'{HOST}/json/ar', timeout=8) as r:
                return json.load(r).get('HIDCTL', {})
        except Exception:
            time.sleep(1)
    return {}


seen = 0
prev = None
t0 = time.time()
last_counts = None
while time.time() - t0 < SECONDS:
    h = hid()
    if not h:
        print('board not answering')
        time.sleep(2)
        continue
    log = h.get('log', [])
    # the log is a ring buffer: it grows until it is full
    if len(log) >= seen and seen:
        for e in log[seen:]:
            b = bytes.fromhex(e['d'])
            mark = ''
            if prev is not None:
                diff = [f'[{j}]{prev[j]:02X}->{b[j]:02X}'
                        for j in range(min(len(prev), len(b))) if prev[j] != b[j]]
                mark = '  CHANGED ' + ', '.join(diff) if diff else ''
            print(f"{time.time() - t0:7.2f} skip={e['s']:>5} "
                  f"{' '.join(f'{x:02X}' for x in b)}{mark}", flush=True)
            prev = b
    seen = len(log)
    counts = (h.get('reports_in'), h.get('wheel_min'), h.get('wheel_max'),
              h.get('wheel_moved'), h.get('wheel'))
    if counts != last_counts:
        print(f"{time.time() - t0:7.2f} reports_in={counts[0]} "
              f"wheel={counts[4]} min={counts[1]} max={counts[2]} "
              f"moved={counts[3]}", flush=True)
        last_counts = counts
    time.sleep(0.2)

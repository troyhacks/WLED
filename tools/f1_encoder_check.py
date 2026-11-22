"""One-shot check of the F1's encoder on the board.

Prints the last raw report, the wheel counters and the eight control
values. Run it once per thing you turn - it makes ONE request, because the
board's web server wedges after roughly 60-70 of them.

    python tools/f1_encoder_check.py [url]
"""
import json, sys, urllib.request

url = sys.argv[1] if len(sys.argv) > 1 else 'http://192.168.6.249'
with urllib.request.urlopen(f'{url}/json/ar', timeout=8) as r:
    h = json.load(r).get('HIDCTL', {})

raw = bytes.fromhex(h['reports'][0]['bytes']) if h.get('reports') else b''
print(f"report   : {' '.join(f'{x:02X}' for x in raw)}")
print(f"byte 5   : {raw[5]:02X}   (the endless encoder's field)")
print(f"buttons  : {raw[1]:02X} {raw[2]:02X} {raw[3]:02X} {raw[4]:02X}")
print(f"values   : {h.get('values')}")
print(f"wheel    : last={h.get('wheel')} min={h.get('wheel_min')} "
      f"max={h.get('wheel_max')} moved={h.get('wheel_moved')}")
print(f"reports  : {h.get('reports_in')}  errors={h.get('in_errors')} zlps={h.get('zlps')}")
print(f"class    : stage={h.get('class_stage')} status={h.get('class_status')}")

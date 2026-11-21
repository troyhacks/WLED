"""Watch byte 5 for a few minutes - for the power-cycle test."""
import json, time, urllib.request
def ar():
    try:
        return json.load(urllib.request.urlopen('http://192.168.6.249/json/ar',timeout=8)).get('HIDCTL',{})
    except Exception:
        return {}
print("watching byte 5 - TURN THE ENCODER", flush=True)
t0=time.time(); shown=set()
while time.time()-t0 < 300:
    h=ar()
    if not h: time.sleep(2); continue
    w=h.get('wheel')
    if w not in shown:
        shown.add(w)
        print(f"  t={time.time()-t0:5.1f}s  byte5={w}  min={h.get('wheel_min')} "
              f"max={h.get('wheel_max')} moved={h.get('wheel_moved')} "
              f"reports={h.get('reports_in')}", flush=True)
    if h.get('wheel_moved'):
        print(f"  *** THE ENCODER MOVED at t={time.time()-t0:5.1f}s ***", flush=True)
    time.sleep(1)

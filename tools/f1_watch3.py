"""A patient watcher for byte 5 - survives a wedged web server."""
import json, time, urllib.request
def ar():
    try:
        return json.load(urllib.request.urlopen('http://192.168.6.249/json/ar',timeout=6)).get('HIDCTL',{})
    except Exception:
        return {}
print("watching byte 5 (poll every 3s) - TURN THE ENCODER", flush=True)
t0=time.time(); shown=set(); last_ok=0
while time.time()-t0 < 900:
    h=ar()
    if h:
        last_ok=time.time()
        w=h.get('wheel')
        if w not in shown:
            shown.add(w)
            print(f"  t={time.time()-t0:6.1f}s  byte5={w}  min={h.get('wheel_min')} "
                  f"max={h.get('wheel_max')} moved={h.get('wheel_moved')} "
                  f"reports={h.get('reports_in')} errs={h.get('in_errors')}", flush=True)
        if h.get('wheel_moved'):
            print(f"  *** ENCODER MOVED at t={time.time()-t0:6.1f}s ***", flush=True)
    else:
        if time.time()-last_ok > 20:
            print(f"  t={time.time()-t0:6.1f}s  (web server busy)", flush=True)
            last_ok=time.time()
    time.sleep(3)

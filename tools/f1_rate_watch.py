"""Watch the report RATE. On a PC it jumps 101 -> 131-139 Hz when the
wheel moves. On the P4 it has been a dead-steady 100.08 Hz."""
import json, time, urllib.request
def ar():
    try:
        return json.load(urllib.request.urlopen('http://192.168.6.249/json/ar',timeout=6)).get('HIDCTL',{})
    except Exception:
        return {}
print("report RATE every 2s - TURN THE ENCODER", flush=True)
t0=time.time(); prev=None; pt=time.time(); pr=0
while time.time()-t0 < 240:
    h=ar()
    if h:
        r=h.get('reports_in',0); t=time.time()
        if t-pt > 2:
            print(f"  t={t-t0:6.1f}s  rate={(r-pr)/(t-pt):6.2f}/s  byte5={h.get('wheel')} "
                  f"moved={h.get('wheel_moved')}", flush=True)
            pt, pr = t, r
    if h: prev=h
    time.sleep(0.7)

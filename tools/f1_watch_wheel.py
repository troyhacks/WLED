import json, time, urllib.request
def ar():
    return json.load(urllib.request.urlopen('http://192.168.6.249/json/ar',timeout=5)).get('HIDCTL',{})
deadline=time.time()+200
h={}
while time.time()<deadline:
    try:
        h=ar()
        if h.get('connected'): break
    except Exception: pass
    time.sleep(3)
print("connected:",h.get('connected'),flush=True)
t0=time.time(); r0=h.get('reports_in',0); z0=h.get('zlps',0)
while time.time()-t0 < 560:
    time.sleep(5)
    try: h=ar()
    except Exception: continue
    dt=time.time()-t0
    print(f"t={dt:5.1f}s  reports={h.get('reports_in')}  zlps={h.get('zlps')}  "
          f"rate={(h.get('reports_in',0)-r0)/dt:6.1f}/s  "
          f"wheel={h.get('wheel')} min={h.get('wheel_min')} max={h.get('wheel_max')} "
          f"moved={h.get('wheel_moved')}  page={h.get('page')}",flush=True)

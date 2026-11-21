import json, time, urllib.request
def ar():
    return json.load(urllib.request.urlopen('http://192.168.6.249/json/ar',timeout=5)).get('HIDCTL',{})
def post(b):
    req=urllib.request.Request('http://192.168.6.249/json/state',data=json.dumps(b).encode(),
                               headers={'Content-Type':'application/json'})
    urllib.request.urlopen(req,timeout=6).read()
print("KEEP TURNING THE ENCODER - this runs 4 minutes", flush=True)
t0=time.time(); last=None
while time.time()-t0 < 240:
    try:
        h=ar()
    except Exception:
        time.sleep(1); continue
    t=time.time()-t0
    if int(t) % 15 == 0 and int(t) != last:
        last=int(t)
        try: post({"HIDCTL":{"classreq":True}})
        except Exception: pass
    print(f"t={t:5.1f} byte5={h.get('wheel')} min={h.get('wheel_min')} max={h.get('wheel_max')} "
          f"moved={h.get('wheel_moved')} 0xDD={h.get('class_data')} status={h.get('class_status')}", flush=True)
    time.sleep(1)

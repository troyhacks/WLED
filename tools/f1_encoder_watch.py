import json, time, urllib.request
def ar():
    return json.load(urllib.request.urlopen('http://192.168.6.249/json/ar',timeout=5)).get('HIDCTL',{})
print("watching byte 5 and the 0xDD answer - TURN THE ENCODER", flush=True)
prev=None
t0=time.time()
while time.time()-t0 < 600:
    try:
        h=ar()
    except Exception:
        time.sleep(2); continue
    sig=(h.get('wheel'), h.get('wheel_min'), h.get('wheel_max'), h.get('class_data'))
    if sig!=prev:
        print(f"  t={time.time()-t0:5.0f}s  byte5={h.get('wheel')} "
              f"min={h.get('wheel_min')} max={h.get('wheel_max')} "
              f"moved={h.get('wheel_moved')}  0xDD answer={h.get('class_data')} "
              f"status={h.get('class_status')}", flush=True)
        prev=sig
    time.sleep(1)

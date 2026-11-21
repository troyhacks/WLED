import json, time, urllib.request
def ar():
    try:
        return json.load(urllib.request.urlopen('http://192.168.6.249/json/ar',timeout=6)).get('HIDCTL',{})
    except Exception:
        return {}
print("watching the feature report + byte 5 - TURN THE ENCODER", flush=True)
prev=None; t0=time.time(); last_feat=None
while time.time()-t0 < 240:
    h=ar()
    if not h: time.sleep(2); continue
    f=h.get('feature')
    sig=(h.get('wheel'), h.get('feature'))
    if sig!=prev:
        fb=bytes.fromhex(f) if f else b''
        diff=[]
        if last_feat and len(last_feat)==len(fb):
            diff=[f"{i:02X}:{last_feat[i]:02X}->{fb[i]:02X}" for i in range(len(fb)) if last_feat[i]!=fb[i]]
        print(f"t={time.time()-t0:5.0f}s byte5={h.get('wheel')} "
              f"min={h.get('wheel_min')} max={h.get('wheel_max')} moved={h.get('wheel_moved')}"
              + (f"  feature changed: {', '.join(diff)}" if diff else ""), flush=True)
        if fb: last_feat=fb
        prev=sig
    time.sleep(1)

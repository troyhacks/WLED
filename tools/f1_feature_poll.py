import json, time, urllib.request
def ar():
    try:
        return json.load(urllib.request.urlopen('http://192.168.6.249/json/ar',timeout=6)).get('HIDCTL',{})
    except Exception:
        return {}
def post(b):
    try:
        req=urllib.request.Request('http://192.168.6.249/json/state',data=json.dumps(b).encode(),
                                   headers={'Content-Type':'application/json'})
        urllib.request.urlopen(req,timeout=8).read()
    except Exception:
        pass
print("re-requesting the feature report every 8s - KEEP TURNING THE ENCODER", flush=True)
last=None; t0=time.time()
while time.time()-t0 < 200:
    post({"HIDCTL":{"classreq":True}})
    time.sleep(2.5)
    h=ar()
    fb=bytes.fromhex(h.get('feature') or '')
    if fb:
        diff=[]
        if last and len(last)==len(fb):
            diff=[f"{i:02X}:{last[i]:02X}->{fb[i]:02X}" for i in range(len(fb)) if last[i]!=fb[i]]
        print(f"t={time.time()-t0:5.0f}s byte5={h.get('wheel')} moved={h.get('wheel_moved')} "
              f"feat[0]={fb[0]:02X} feat[1]={fb[1]:02X}"
              + (f"  CHANGED: {', '.join(diff)}" if diff else "  (unchanged)"), flush=True)
        last=fb
    time.sleep(5)

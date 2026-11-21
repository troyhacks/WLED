"""Re-read the 63-byte feature report and report EVERY byte that changes.

If the blob is live state, a KNOB turn will move bytes in it. If it never
moves, it is a static block (firmware or a preset), and the encoder is
not in it.
"""
import json, time, urllib.request
def ar():
    try:
        return json.load(urllib.request.urlopen('http://192.168.6.249/json/ar',timeout=8)).get('HIDCTL',{})
    except Exception:
        return {}
def post(b):
    try:
        req=urllib.request.Request('http://192.168.6.249/json/state',data=json.dumps(b).encode(),
                                   headers={'Content-Type':'application/json'})
        urllib.request.urlopen(req,timeout=8).read()
    except Exception:
        pass
print("TURN A KNOB (and then the encoder) - re-reading the feature report", flush=True)
last=None; t0=time.time()
while time.time()-t0 < 220:
    post({"HIDCTL":{"classreq":True}})
    time.sleep(1.2)
    h=ar()
    fb=bytes.fromhex(h.get('feature') or '')
    if fb:
        if last and len(last)==len(fb):
            diff=[f"{i}:{last[i]:02X}->{fb[i]:02X}" for i in range(len(fb)) if last[i]!=fb[i]]
            if diff:
                print(f"  t={time.time()-t0:5.0f}s  FEATURE CHANGED: {', '.join(diff)}", flush=True)
        else:
            print(f"  t={time.time()-t0:5.0f}s  feature ({len(fb)} bytes): {fb.hex(' ')}", flush=True)
        last=fb
    time.sleep(0.8)

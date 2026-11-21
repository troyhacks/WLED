"""Ask the F1 for its 0xDD vendor byte over and over and print it.

Windows asked once at enumeration and got 0x00. If that byte is the
encoder's position, it will move when the encoder is turned - and it is
the only thing the device has ever answered us with.
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
print("asking 0xDD repeatedly - TURN THE ENCODER", flush=True)
seen=set(); t0=time.time()
while time.time()-t0 < 180:
    post({"HIDCTL":{"classreq":True}})
    time.sleep(1.5)
    h=ar()
    d=h.get('class_data')
    key=(d, h.get('wheel'))
    if key not in seen:
        seen.add(key)
        print(f"  t={time.time()-t0:5.0f}s  0xDD answer={d}  byte5={h.get('wheel')} "
              f"moved={h.get('wheel_moved')}", flush=True)
    time.sleep(0.5)

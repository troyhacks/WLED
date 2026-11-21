"""Re-read the feature reports continuously while the encoder is turned.

The switch re-sends the whole class sequence, which includes the
GET_REPORT for the F1's 0xD0 (63 bytes of device state) and 0xF0 (the
knob and fader positions). If the device only refreshes its input report
when it serves one of those, byte 5 will start moving here.
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
print("hammering the class/feature requests - TURN THE ENCODER", flush=True)
t0=time.time(); seen=set()
while time.time()-t0 < 200:
    post({"HIDCTL":{"classreq":True}})
    time.sleep(1.5)
    h=ar()
    w=h.get('wheel')
    if w not in seen:
        seen.add(w)
        print(f"t={time.time()-t0:5.0f}s  byte5={w}  min={h.get('wheel_min')} "
              f"max={h.get('wheel_max')} moved={h.get('wheel_moved')} "
              f"feature[1]={(h.get('feature') or '00')[2:4]}", flush=True)

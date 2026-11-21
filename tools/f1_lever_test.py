"""Three phases, each a different lever, while the encoder is turned.

  A  painting OFF, other interface RELEASED   (a PC's behaviour exactly)
  B  painting ON,  other interface RELEASED   (only the LED report)
  C  painting ON,  other interface CLAIMED    (what we do now)

The polling is sparse on purpose: this board's web server wedges after
about sixty requests, and a 503 is not a failure of the test.
"""
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
        return True
    except Exception:
        return False

deadline=time.time()+600
while time.time()<deadline:
    h=ar()
    if h.get('connected'): break
    time.sleep(5)
print(f"connected. other_iface={h.get('other_iface')} claimed={h.get('other_claimed')} "
      f"reports={h.get('reports_in')}", flush=True)

PHASES=[("A",{"HIDCTL":{"paint":False,"otheriface":False}}),
        ("B",{"HIDCTL":{"paint":True ,"otheriface":False}}),
        ("C",{"HIDCTL":{"paint":True ,"otheriface":True }})]
for name,body in PHASES:
    post(body); time.sleep(2)
    h=ar()
    print(f"\n=== PHASE {name}: {body['HIDCTL']} "
          f"(other iface claimed={h.get('other_claimed')}) ===", flush=True)
    print(">>> TURN THE ENCODER - watch the F1's own display <<<\n", flush=True)
    t0=time.time(); moved=False; last=None
    while time.time()-t0 < 60:
        time.sleep(6)
        h=ar()
        if not h: print("   (web server busy)", flush=True); continue
        w=h.get('wheel')
        if w!=last:
            print(f"  t={time.time()-t0:5.1f}s byte5={w} min={h.get('wheel_min')} "
                  f"max={h.get('wheel_max')} moved={h.get('wheel_moved')}", flush=True)
            last=w
        if h.get('wheel_moved'): moved=True
    print(f"  --> PHASE {name}: moved={moved} min={h.get('wheel_min')} "
          f"max={h.get('wheel_max')}", flush=True)

post({"HIDCTL":{"paint":True,"otheriface":True}})
print("\ndone - back to the normal setup", flush=True)

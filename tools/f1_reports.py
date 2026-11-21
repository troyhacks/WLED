import json, time, urllib.request
def ar():
    return json.load(urllib.request.urlopen('http://192.168.6.249/json/ar',timeout=5)).get('HIDCTL',{})
deadline=time.time()+150
while time.time()<deadline:
    try:
        h=ar()
        if h.get('connected'): break
    except Exception: pass
    time.sleep(4)
print("connected:", h.get('connected'))
prev=None
for i in range(40):
    h=ar()
    sig=json.dumps(h.get('reports'),sort_keys=True)
    if sig!=prev:
        print(f"--- t={i*2}s  wheel_min={h.get('wheel_min')} wheel_max={h.get('wheel_max')} moved={h.get('wheel_moved')}")
        for r in h.get('reports',[]):
            print(f"    id=0x{r['id']:02X} len={r['len']:2} count={r['count']:6}  {r['bytes']}")
        prev=sig
    time.sleep(2)

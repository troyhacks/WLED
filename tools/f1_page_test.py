import json, time, urllib.request
def ar():
    return json.load(urllib.request.urlopen('http://192.168.6.249/json/ar',timeout=5))
def post(body):
    req=urllib.request.Request('http://192.168.6.249/json/state',
        data=json.dumps(body).encode(), headers={'Content-Type':'application/json'})
    return urllib.request.urlopen(req,timeout=6).read()
deadline=time.time()+150
while time.time()<deadline:
    try:
        d=ar()
        if d.get('HIDCTL',{}).get('connected'): break
    except Exception: pass
    time.sleep(5)
print("connected:", ar()['HIDCTL']['connected'])
for pg in (3, 9, 10, 16, 1):
    post({"HIDCTL":{"page":pg}})
    time.sleep(0.6)
    h=ar()['HIDCTL']
    raw=bytes.fromhex(h['led_report'])
    print(f"page {h['page']:2}  display: {' '.join(f'{x:02X}' for x in raw[1:17])}")

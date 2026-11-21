import json, time, urllib.request
def ar():
    try:
        return json.load(urllib.request.urlopen('http://192.168.6.249/json/ar',timeout=6)).get('HIDCTL',{})
    except Exception:
        return {}
deadline=time.time()+300
while time.time()<deadline:
    h=ar()
    if h.get('connected'): break
    time.sleep(5)
print("connected:",h.get('connected'),flush=True)
for k in ('class_stage','class_status','class_data','feature_len','other_iface','other_claimed','wheel','reports_in'):
    print(f"  {k:14} {h.get(k)}",flush=True)
f=h.get('feature')
if f:
    b=bytes.fromhex(f)
    print(f"  feature report ({len(b)} bytes):",flush=True)
    for i in range(0,len(b),16):
        print(f"    {i:02X}: "+" ".join(f"{x:02X}" for x in b[i:i+16]),flush=True)

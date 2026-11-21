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
print("connected:",h.get('connected'))
for k in ('class_status','class_stage','class_data','wheel','wheel_min','wheel_max','wheel_moved','reports_in','zlps','page','active_preset'):
    print(f"  {k:14} {h.get(k)}")

"""Pull the board's own wire log and crunch it.

The board keeps every report that DIFFERED from the one before, with the
number of packets that came in between. That is the data to crunch: which
byte moves when the encoder is turned, and whether the zero-length
packets are where they should be.
"""
import json, sys, time, urllib.request
def ar():
    try:
        return json.load(urllib.request.urlopen('http://192.168.6.249/json/ar',timeout=8)).get('HIDCTL',{})
    except Exception:
        return {}
deadline=time.time()+300
while time.time()<deadline:
    h=ar()
    if h.get('connected') and h.get('log'): break
    time.sleep(5)
h=ar()
print(f"connected={h.get('connected')} class_stage={h.get('class_stage')} "
      f"class_status={h.get('class_status')} feature_len={h.get('feature_len')}")
print(f"log_reports={h.get('log_reports')} log_zlps={h.get('log_zlps')} entries={len(h.get('log',[]))}")
print()
entries=h.get('log',[])
print(f"{'#':>3} {'skipped':>7}  bytes 1..21 (byte 0 is the report ID)")
prev=None
for i,e in enumerate(entries):
    b=bytes.fromhex(e['d'])
    row=' '.join(f"{x:02X}" for x in b[:22])
    mark=""
    if prev is not None:
        diff=[f"{j}:{prev[j]:02X}->{b[j]:02X}" for j in range(min(len(prev),len(b))) if prev[j]!=b[j]]
        if diff: mark="  CHANGED "+", ".join(diff)
    print(f"{i:>3} {e['s']:>7}  {row}{mark}")
    prev=b

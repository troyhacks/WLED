import json, time, urllib.request
def ar():
    return json.load(urllib.request.urlopen('http://192.168.6.249/json/ar',timeout=5)).get('HIDCTL',{})
deadline=time.time()+120
while time.time()<deadline:
    try:
        h=ar()
        if h.get('connected') and h.get('config_desc'): break
    except Exception: pass
    time.sleep(4)
raw=bytes.fromhex(h.get('config_desc',''))
print(f"config descriptor: {len(raw)} bytes")
for i in range(0,len(raw),16):
    print(f"  {i:04X}: " + " ".join(f"{x:02X}" for x in raw[i:i+16]))
print()
i=0
while i < len(raw):
    b=raw[i]
    t=raw[i+1] if i+1<len(raw) else 0
    body=raw[i+2:i+b]
    if b==0: break
    if t==2:
        print(f"CONFIG  len={b} wTotalLength={int.from_bytes(body[0:2],'little')} "
              f"numInterfaces={body[2]} cfgValue={body[3]} iConfiguration={body[4]} "
              f"attributes=0x{body[5]:02X} maxPower={body[6]}")
    elif t==4:
        print(f"  IFACE num={body[0]} alt={body[1]} eps={body[2]} class={body[3]} "
              f"sub={body[4]} proto={body[5]}")
    elif t==5:
        print(f"    EP  addr=0x{body[0]:02X} attr=0x{body[1]:02X} mps={int.from_bytes(body[2:4],'little')}")
    elif t==0x21:
        print(f"    HID bcd={int.from_bytes(body[0:2],'little'):04X} country={body[2]} "
              f"numdesc={body[3]} type=0x{body[4]:02X} len={int.from_bytes(body[5:7],'little')}")
    else:
        print(f"    type=0x{t:02X} len={b} : {body.hex(' ')}")
    i+=b

"""Probe the Soomfon CN003 on the desktop: read its reports, talk to it.

This is the experiment the board could not do: the deck is a HID device
here, so its reports can be read directly, and the ASCII protocol can be
spoken to it.

    python tools/deck_probe.py [seconds]
"""
import sys
import time
import hid

VID, PID = 0x1500, 0x3002


def find():
    """Interface 0 is the vendor one (usage page 0xFFA0); 1 is the keyboard."""
    for d in hid.enumerate():
        if d.get('vendor_id') == VID and d.get('product_id') == PID:
            if d.get('usage_page') == 0xFFA0:
                return d
    return None


def cmd(mnemonic: bytes, payload: bytes = b'', total: int = 1025) -> bytes:
    """The deck's outbound framing: 00 43 52 54 00 00 | mnemonic | payload."""
    pkt = bytes([0x00, 0x43, 0x52, 0x54, 0x00, 0x00]) + mnemonic + payload
    return pkt.ljust(total, b'\x00')


def send(h, mnem, payload=b'', total=1025):
    pkt = cmd(mnem, payload, total)
    n = h.write(pkt)
    print(f"  sent {mnem.decode()} ({len(pkt)} bytes, {n} accepted)")
    return n


def show(data):
    print(f"    len={len(data)}")
    print("    " + ' '.join(f'{x:02X}' for x in data[:48]) +
          (" ..." if len(data) > 48 else ""))
    if len(data) >= 11:
        print(f"    magic={data[0:3]!r} msg_id=0x{data[9]:02X} state=0x{data[10]:02X}")
        print(f"    bitmap 11..26: {' '.join(f'{x:02X}' for x in data[11:27])}")


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0
    dev = find()
    if dev is None:
        raise SystemExit("the CN003 is not on this machine")
    print(f"found: {dev['path']}")
    h = hid.device()
    h.open_path(dev['path'])
    print("opened; reading")
    t0 = time.time()
    got = 0
    while time.time() - t0 < secs:
        try:
            d = h.read(512, timeout_ms=200)
        except Exception as e:
            print("  read error:", e)
            break
        if not d:
            continue
        got += 1
        print(f"--- report {got} at t={time.time() - t0:.2f}s")
        show(d)
    print(f"{got} reports in {secs}s while idle")

    # Now talk to it: DIS, the backlight, and the keepalive. The device may
    # answer, and the keys should light up.
    print("sending DIS (display reset) ...")
    send(h, b'DIS')
    time.sleep(0.4)
    print("sending LIG 100 (key backlight) ...")
    send(h, b'LIG', bytes([0x00, 0x00, 100]))
    time.sleep(0.4)
    print("sending CONNECT (keepalive) ...")
    send(h, b'CONNECT')
    time.sleep(0.4)
    print("reading for 8 s after the commands ...")
    t0 = time.time()
    while time.time() - t0 < 8:
        try:
            d = h.read(512, timeout_ms=200)
        except Exception as e:
            print("  read error:", e)
            break
        if not d:
            continue
        got += 1
        print(f"--- report {got}")
        show(d)
    print(f"{got} reports in total")
    h.close()


if __name__ == '__main__':
    main()

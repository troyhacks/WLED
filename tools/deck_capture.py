"""Capture the USB traffic of the CN003 while the deck is driven.

This is the ground truth: the same deck, the same commands, the same
bytes - but seen on the wire from a host that works.

    python tools/deck_capture.py [seconds]

The deck must be plugged into THIS machine (not the P4). The script
starts tshark on the USBPcap interface, drives the deck with the same
commands the P4 sends, and prints the OUT packets that went to it.
"""
import subprocess
import sys
import threading
import time
import os

TSHARK = r"C:\Program Files\Wireshark\tshark.exe"
CAPFILE = "build_output/deck_wire.pcapng"


def tshark_interfaces():
    out = subprocess.run([TSHARK, '-D'], capture_output=True, text=True).stdout
    names = []
    for l in out.splitlines():
        if 'USBPcap' not in l:
            continue
        # "10. \\.\USBPcap1 (USBPcap1)" -> the interface is what follows the
        # number and the dot.
        head, _, tail = l.partition('. ')
        name = tail.split(' (')[0].strip()
        if name:
            names.append(name)
    return names


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
    ifaces = tshark_interfaces()
    if not ifaces:
        raise SystemExit("no USBPcap interface found")
    print("capturing on", ifaces[0])

    cap = subprocess.Popen(
        [TSHARK, '-i', ifaces[0], '-w', CAPFILE],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    time.sleep(2)
    # Drive the deck exactly as the P4 does.
    sys.argv = ['deck_session.py', '--push', '--read', str(secs)]
    import deck_session
    deck_session.main()

    time.sleep(2)
    cap.terminate()
    cap.wait(timeout=10)
    if not os.path.exists(CAPFILE):
        raise SystemExit("no capture file")
    print(f"\ncapture: {CAPFILE}")
    out = subprocess.run(
        [TSHARK, '-r', CAPFILE, '-T', 'fields',
         '-e', 'frame.number', '-e', 'usb.transfer_type',
         '-e', 'usb.endpoint_address', '-e', 'usb.data_len', '-e', 'usb.capdata'],
        capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split('\t')
        if len(parts) < 5:
            continue
        num, xfer, ep, dlen, data = parts[:5]
        try:
            b = bytes.fromhex(data.replace(':', ''))
        except ValueError:
            continue
        # The OUT endpoint is 0x03; the IN endpoint is 0x82.
        if ep.lower().endswith('03') or dlen in ('1024', '1025'):
            print(f"#{num} OUT ep={ep} len={dlen}: {b[:24].hex(' ')}")


if __name__ == '__main__':
    main()

"""Read a USB capture of the CN003 and show what the host sent it.

    python tools/deck_wire.py <capture.pcapng>

Prints every packet to the deck's OUT endpoint (0x03) and its IN
endpoint (0x82), with the size and the head of the data - so the framing
can be compared with what the P4 sends.
"""
import subprocess
import sys
from collections import Counter

TSHARK = r"C:\Program Files\Wireshark\tshark.exe"


def main():
    path = sys.argv[1]
    out = subprocess.run(
        [TSHARK, '-r', path, '-T', 'fields',
         '-e', 'frame.number', '-e', 'usb.transfer_type',
         '-e', 'usb.endpoint_address', '-e', 'usb.data_len', '-e', 'usb.capdata'],
        capture_output=True, text=True).stdout

    rows = []
    for line in out.splitlines():
        parts = line.split('\t')
        if len(parts) < 5:
            continue
        num, xfer, ep, dlen, data = parts[:5]
        try:
            b = bytes.fromhex(data.replace(':', ''))
        except ValueError:
            b = b''
        rows.append((num, xfer, ep, dlen, b))

    # Only the deck's endpoints matter.
    deck = [r for r in rows if r[2].lower().endswith('03') or r[2].lower().endswith('82')]
    if not deck:
        deck = rows

    sizes = Counter(r[3] for r in deck)
    print("packet sizes:", dict(sizes))
    print(f"{len(deck)} packets\n")
    shown = 0
    for num, xfer, ep, dlen, b in deck:
        if shown >= 40:
            print("...")
            break
        shown += 1
        head = ' '.join(f'{x:02X}' for x in b[:20])
        print(f"#{num} type={xfer} ep={ep} len={dlen}: {head}")
        if b[:3] == b'CRT':
            # The command word: bytes 6.. are the mnemonic.
            mnem = b[6:12].decode('ascii', 'replace')
            print(f"      mnemonic='{mnem}'  first 40 bytes: "
                  f"{' '.join(f'{x:02X}' for x in b[:40])}")


if __name__ == '__main__':
    main()

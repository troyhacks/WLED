"""Decode the F1's USB captures: what the PC did, and what the device sent.

Reads a USBPcap capture with tshark and prints, for every IN report from
the F1, the bytes that differ from the previous one - and separately, the
traffic the host sent (control transfers and OUT packets).

    python tools/f1_capture_analyze.py usb2.pcapng [more captures...]
"""
import subprocess, sys, collections

TSHARK = r"C:\Program Files\Wireshark\tshark.exe"


def packets(path):
    cmd = [TSHARK, '-r', path, '-T', 'fields',
           '-e', 'frame.number', '-e', 'frame.time_relative',
           '-e', 'usb.transfer_type', '-e', 'usb.endpoint_address',
           '-e', 'usb.data_len', '-e', 'usb.capdata',
           '-E', 'separator=|']
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    for line in out.splitlines():
        f = line.split('|')
        if len(f) < 6:
            continue
        num, t, xfer, ep, dlen, data = f[0], f[1], f[2], f[3], f[4], f[5]
        try:
            b = bytes.fromhex(data.replace(':', ''))
        except ValueError:
            b = b''
        yield num, float(t or 0), xfer, ep, dlen, b


def main():
    for path in sys.argv[1:]:
        print(f"\n================ {path}")
        ins = []
        ctl = collections.Counter()
        outs = []
        for num, t, xfer, ep, dlen, b in packets(path):
            if xfer == '0x01' and ep == '0x81':
                ins.append((num, t, b))
            elif xfer == '0x02':
                ctl[(ep, b[:8].hex())] += 1
            else:
                outs.append((num, t, ep, b))
        print(f"IN reports: {len(ins)}")
        if ins:
            lens = collections.Counter(len(b) for _, _, b in ins)
            print(f"  lengths: {dict(lens)}")
            base = ins[0][2]
            print(f"  first  : {' '.join(f'{x:02X}' for x in base)}")
            varying = collections.Counter()
            prev = None
            full = [b for _, _, b in ins if len(b) >= 22]
            for b in full:
                if prev is not None:
                    for j in range(min(len(prev), len(b))):
                        if prev[j] != b[j]:
                            varying[j] += 1
                prev = b
            print("  byte index -> how many reports it changed in:")
            for j, n in sorted(varying.items()):
                vals = {b[j] for b in full if j < len(b)}
                extra = ''
                if len(vals) <= 10:
                    extra = f'  values={sorted(vals)}'
                print(f"    byte {j:>2}: {n:>6} changes{extra}")
            # the encoder's byte, over time
            print("  byte 5 over time (the encoder field):")
            runs = []
            for b in full:
                v = b[5]
                if runs and runs[-1][0] == v:
                    runs[-1][1] += 1
                else:
                    runs.append([v, 1])
            print('    ' + ' '.join(f'{v:02X}x{n}' for v, n in runs[:60]))
        print(f"control transfers: {sum(ctl.values())}")
        for (ep, d), n in ctl.most_common(12):
            print(f"    ep={ep} n={n} data={d}")
        print(f"other/OUT packets: {len(outs)}")
        for num, t, ep, b in outs[:12]:
            print(f"    #{num} t={t:.3f} ep={ep} len={len(b)} {' '.join(f'{x:02X}' for x in b[:16])}")


if __name__ == '__main__':
    main()

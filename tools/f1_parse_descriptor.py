"""Parse the F1's own HID report descriptor into items.

The board fetched the descriptor from the device (GET_DESCRIPTOR 0x2200 on
interface 0) and logged it. This parses that dump so every input field is
listed with its usage, bit size and byte offset - the device's own
declaration of where the rotary encoder is.

    python tools/f1_parse_descriptor.py <console log> [more logs...]
"""
import sys, re

USAGE = {
    0x00: '?', 0x01: 'Pointer/Physical', 0x02: 'Button', 0x03: 'Endless encoder',
    0x04: 'Knob', 0x05: 'Fader', 0x80: 'LED brightness', 0x81: 'LED?',
    0xD1: 'usage 209', 0xF1: 'usage 241',
}


def dumps(path):
    """Every 'descriptor 2200' hex dump in a console log."""
    out = []
    cur = None
    for line in open(path, 'r', encoding='utf-8', errors='replace'):
        if 'descriptor 2200' in line:
            if cur:
                out.append(cur)
            cur = []
            continue
        m = re.match(r'^\s+[0-9A-F]{4}: ((?:[0-9A-F]{2} ?)+)\s*$', line)
        if m and cur is not None:
            cur += [int(x, 16) for x in m.group(1).split()]
    if cur:
        out.append(cur)
    return out


def value(d, i):
    """A short (1 byte) or long (2 byte) descriptor value."""
    size = d[i]
    if size == 2:
        return d[i + 2]
    return d[i + 2] | (d[i + 3] << 8)


def parse(d):
    """Walk the descriptor, tracking bit offsets per report."""
    i = 0
    usage = 0
    report_id = 0
    bit = 0            # bit offset in the current report
    size_bits = 8
    count = 1
    rows = []
    while i < len(d):
        size = d[i]
        if size == 0:
            break
        if i + size > len(d):
            rows.append(f"  (truncated at offset {i})")
            break
        t = d[i + 1]
        if t == 0x05:                      # Usage Page
            pass
        elif t == 0x09:                    # Usage
            usage = value(d, i)
        elif t == 0x85:                    # Report ID
            report_id = d[i + 2]
            bit = 0
        elif t == 0x75:                    # Report Size
            size_bits = d[i + 2]
        elif t == 0x95:                    # Report Count
            count = d[i + 2]
        elif t == 0xA1:                    # Collection
            rows.append(f"  Collection usage={USAGE.get(usage, hex(usage))}")
        elif t == 0xC0:                    # End Collection
            rows.append("  End Collection")
        elif t in (0x81, 0x91, 0xB1):      # Input / Output / Feature
            kind = {0x81: 'Input ', 0x91: 'Output', 0xB1: 'Feature'}[t]
            start = bit
            rows.append(
                f"  {kind} report_id=0x{report_id:02X} "
                f"usage={USAGE.get(usage, hex(usage))} "
                f"bits={size_bits} count={count} "
                f"-> bytes {start // 8}..{(start + size_bits * count - 1) // 8} "
                f"({size_bits * count} bits)")
            bit += size_bits * count
        i += size
    return rows


for path in sys.argv[1:]:
    for n, d in enumerate(dumps(path)):
        if len(d) < 32:
            print(f"\n=== {path} dump #{n}: {len(d)} bytes (truncated, skipped)")
            continue
        print(f"\n=== {path} dump #{n}: {len(d)} bytes")
        for r in parse(d):
            print(r)

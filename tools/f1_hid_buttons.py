#!/usr/bin/env python3
"""List every button press in an F1 HID capture, in order.

Bytes 1..4 of the 22-byte report are button bitmaps (MSB first):
byte 1 -> bits 7..0, byte 2 -> bits 7..0, and so on. This prints the
press/release sequence so the physical controls can be labelled.

Usage:
    python tools/f1_hid_buttons.py tools/f1_reports_raw.log
"""
import sys


def load(path):
    out = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            parts = line.split()
            if len(parts) < 2:
                continue
            try:
                t = float(parts[0])
                data = bytes(int(b, 16) for b in parts[1:])
            except ValueError:
                continue
            out.append((t, data))
    return out


def buttons(d):
    """Return the set of (byte, bit) currently held."""
    held = set()
    for b in range(1, 5):
        if len(d) <= b:
            break
        v = d[b]
        for bit in range(8):
            if v & (0x80 >> bit):
                held.add((b, bit))
    return held


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "tools/f1_reports_raw.log"
    reports = load(path)
    print(f"{len(reports)} reports\n=== press sequence ===")
    prev = set()
    n = 0
    for t, d in reports:
        cur = buttons(d)
        for item in sorted(cur - prev):
            n += 1
            print(f"  {n:3d}. {t:8.3f}s  PRESS   byte{item[0]} bit{7 - item[1]} "
                  f"(mask {0x80 >> item[1]:#04x})")
        for item in sorted(prev - cur):
            print(f"          {t:8.3f}s  release byte{item[0]} bit{7 - item[1]}")
        prev = cur
    print(f"\n{n} presses total")


if __name__ == "__main__":
    main()

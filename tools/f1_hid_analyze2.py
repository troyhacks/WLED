#!/usr/bin/env python3
"""Second-stage analysis of the F1 HID reports: knob fields vs button fields.

The 8 knobs are 12-bit little-endian values at byte offsets 6, 8, 10, 12,
14, 16, 18, 20 (value = b[off] | (b[off+1] & 0x0F) << 8). This tool
separates reports where a knob moved from reports where only the other
bytes changed -- i.e. button events -- and prints the raw bytes so the
button bitmap can be read off.

Usage:
    python tools/f1_hid_analyze2.py tools/f1_reports_raw.log [--buttons]
"""
import sys

KNOB_OFFSETS = (6, 8, 10, 12, 14, 16, 18, 20)


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


def knobs(d):
    return tuple(d[o] | ((d[o + 1] & 0x0F) << 8) for o in KNOB_OFFSETS)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    path = args[0] if args else "tools/f1_reports_raw.log"
    reports = load(path)
    print(f"{len(reports)} reports\n")

    print("=== knob values (12-bit at offsets 6,8,10,12,14,16,18,20) ===")
    prev_k = None
    for t, d in reports:
        k = knobs(d)
        if k != prev_k:
            changed = [i for i in range(8) if prev_k is None or k[i] != prev_k[i]]
            print(f"  {t:8.3f}s  {list(k)}  <- knob(s) {changed}")
            prev_k = k

    print("\n=== button events (no knob moved, other bytes changed) ===")
    prev = None
    prev_k = None
    for t, d in reports:
        k = knobs(d)
        if prev is not None and k == prev_k:
            diff = {j: (prev[j], d[j]) for j in range(min(len(d), len(prev)))
                    if prev[j] != d[j]}
            if diff:
                print(f"  {t:8.3f}s  " + "  ".join(
                    f"b{j}:{a:02x}->{b:02x}" for j, (a, b) in sorted(diff.items())))
                print(f"           full: {d.hex(' ')}")
        prev, prev_k = d, k

    print("\n=== bytes 1..5 value histogram ===")
    for j in range(1, 6):
        vals = {}
        for _, d in reports:
            if len(d) > j:
                vals[d[j]] = vals.get(d[j], 0) + 1
        top = sorted(vals.items(), key=lambda kv: -kv[1])[:12]
        print(f"  byte {j}: {top}")


if __name__ == "__main__":
    main()

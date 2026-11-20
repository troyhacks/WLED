#!/usr/bin/env python3
"""Probe the Traktor Kontrol F1's raw HID interface.

The F1 (VID 0x17CC, PID 0x1120) exposes a USB HID interface (class 3) plus
a DFU interface. It has NO class-compliant MIDI endpoint -- NI's driver is
what normally turns its HID reports into MIDI. This probe dumps the raw
reports so the layout can be decoded without any driver installed.

Usage:
    python tools/f1_hid_probe.py --list
    python tools/f1_hid_probe.py --seconds 20
"""
import argparse
import sys
import time

try:
    import hid
except ImportError:
    sys.exit("hidapi not installed: python -m pip install hidapi")

VID = 0x17CC
PID = 0x1120


def enumerate_all():
    """List every HID device Windows knows about."""
    rows = []
    for d in hid.enumerate():
        rows.append(d)
    return rows


def find_f1():
    """Return the device info dict for the F1's HID interface, or None."""
    for d in enumerate_all():
        if d["vendor_id"] == VID and d["product_id"] == PID:
            return d
    return None


def describe(d):
    print(f"path         : {d['path']}")
    print(f"vid:pid      : {d['vendor_id']:#06x}:{d['product_id']:#06x}")
    print(f"usage_page   : {d['usage_page']}")
    print(f"usage        : {d['usage']}")
    print(f"interface    : {d.get('interface_number')}")
    print(f"manufacturer : {d['manufacturer_string']!r}")
    print(f"product      : {d['product_string']!r}")
    print(f"serial       : {d['serial_number']!r}")


def cmd_list():
    print("=== all HID devices ===")
    for d in enumerate_all():
        print(f"{d['vendor_id']:#06x}:{d['product_id']:#06x} up={d['usage_page']} "
              f"u={d['usage']} if={d.get('interface_number')} "
              f"{d['product_string']!r} {d['path']}")
    print()
    f1 = find_f1()
    print("=== F1 ===")
    if f1:
        describe(f1)
    else:
        print("F1 HID interface NOT FOUND")


def cmd_probe(seconds, outfile, probe_writes=False):
    f1 = find_f1()
    if not f1:
        sys.exit("F1 HID interface not found -- is it plugged in?")

    dev = hid.device()
    try:
        dev.open_path(f1["path"])
    except OSError as e:
        sys.exit(f"open failed: {e} -- another app (Traktor? NI driver?) may hold it")

    print("=== open device ===")
    print(f"  path: {f1['path']}")
    for label, fn in (("manufacturer", dev.get_manufacturer_string),
                      ("product", dev.get_product_string),
                      ("serial", dev.get_serial_number_string)):
        try:
            print(f"  {label}: {fn()!r}")
        except Exception as e:  # noqa: BLE001 - diagnostic tool
            print(f"  {label}: <{e}>")
    for label, fn in (("usage_page", getattr(dev, "get_usage_page", None)),
                      ("usage", getattr(dev, "get_usage", None)),
                      ("interface_number", getattr(dev, "get_interface_number", None))):
        if fn is None:
            continue
        try:
            print(f"  {label}: {fn()}")
        except Exception as e:  # noqa: BLE001 - diagnostic tool
            print(f"  {label}: <{e}>")

    # Feature/output report sizes are not exposed by hidapi on Windows, so
    # probe the sizes the device is willing to accept. Off by default:
    # sending junk SET_REPORTs at an unknown device is not something to do
    # casually.
    if probe_writes:
        print("\n=== report size probe (SET_REPORT) ===")
        for size in (8, 9, 16, 17, 20, 32, 33, 64):
            ok = None
            for rid in (0x00, 0x01, 0x02, 0x03):
                try:
                    dev.send_feature_report([rid] + [0] * (size - 1))
                    ok = rid
                    break
                except OSError:
                    continue
            print(f"  feature report size {size}: {'ok (report id 0x%02x)' % ok if ok is not None else 'refused'}")
            if ok is not None:
                break

    print(f"\n=== reading reports for {seconds}s ===")
    print("(turn knobs / press buttons now)")
    prev = None
    t0 = time.time()
    n = 0
    fh = open(outfile, "a", encoding="utf-8") if outfile else None
    try:
        while time.time() - t0 < seconds:
            data = dev.read(64, 200)
            if not data:
                continue
            n += 1
            raw = bytes(data)
            changed = ""
            if prev is not None and len(prev) == len(raw):
                diffs = [f"b{i}:{prev[i]:02x}->{raw[i]:02x}"
                         for i in range(len(raw)) if prev[i] != raw[i]]
                changed = "  changed=" + ",".join(diffs)
            ts = time.time() - t0
            line = f"{ts:8.3f}s len={len(raw):3d} {raw.hex(' ')}{changed}"
            print(line)
            if fh:
                fh.write(f"{ts:8.3f} {raw.hex(' ')}\n")
                fh.flush()
            prev = raw
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        if fh:
            fh.close()
        dev.close()
    print(f"\n{n} reports received")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true", help="enumerate and exit")
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--out", help="append raw reports to this file")
    ap.add_argument("--probe-writes", action="store_true",
                    help="probe SET_REPORT sizes (sends junk to the device)")
    args = ap.parse_args()

    if args.list:
        cmd_list()
    else:
        cmd_probe(args.seconds, args.out, args.probe_writes)


if __name__ == "__main__":
    main()

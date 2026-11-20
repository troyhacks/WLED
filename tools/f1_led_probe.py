#!/usr/bin/env python3
"""Find the F1's LED/display output format, by trying it and asking.

The F1's output report (report ID 0x80) carries the LED brightnesses and
the two seven-segment digits, but the sources disagree on the details: the
data length (80 or 81 bytes) and whether it is an Output or a Feature
report. This walks the candidates, lighting EVERYTHING each time, and asks
whether it lit.

Run it, then look at the controller and press Enter: 'y' if the pads lit,
'n' if not (and it moves on to the next candidate).

Usage:
    python tools/f1_led_probe.py
"""
import sys
import time

try:
    import hid
except ImportError:
    sys.exit("hidapi not installed")

F1_VID, F1_PID = 0x17CC, 0x1120
REPORT_ID = 0x80

# The layout the sources give, in data bytes (no report ID byte):
#   0..15   the two seven-segment digits (8 strokes each)
#   16..23  the special buttons (browse..sync)
#   24..71  the 16 pads, three bytes each: blue, red, green
#   72..79  the four lower buttons, two sides each
def build(data_len):
    """A report that lights everything: pads red, buttons white, '88'."""
    buf = bytearray(data_len)
    # The display: both digits show 8 (all strokes on).
    for i in range(16):
        buf[i] = 0x7F
    # The special buttons: on.
    for i in range(16, 24):
        buf[i] = 0x7F
    # The lower buttons: on.
    for i in range(72, min(80, data_len)):
        buf[i] = 0x7F
    # The pads: bright red (blue, red, green order).
    for p in range(16):
        base = 24 + p * 3
        if base + 2 < data_len:
            buf[base + 0] = 0x00   # blue
            buf[base + 1] = 0x7F   # red
            buf[base + 2] = 0x00   # green
    return bytes(buf)


def find_device():
    for d in hid.enumerate():
        if d["vendor_id"] == F1_VID and d["product_id"] == F1_PID:
            return d["path"]      # hidapi wants the bytes path
    return None


def main():
    path = find_device()
    if not path:
        sys.exit("F1 not found - plug it into this PC")
    dev = hid.device()
    dev.open_path(path)
    print(f"opened {path}\n")

    # (data length, how to send it)
    candidates = [
        (80, "output"),
        (81, "output"),
        (80, "feature"),
        (81, "feature"),
    ]

    # Blinker mode: light everything for a few seconds per candidate, in a
    # fixed order, so it can be watched from across the desk.
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 2
    for r in range(rounds):
        for n, (data_len, kind) in enumerate(candidates):
            data = bytes([REPORT_ID]) + build(data_len)
            try:
                if kind == "output":
                    dev.send_output_report(data)
                else:
                    dev.send_feature_report(data)
                sent = "sent"
            except Exception as e:  # noqa: BLE001 - the point is to show the error
                sent = f"FAILED ({e})"
            print(f"round {r} candidate {n + 1}: {kind} report, {data_len} data bytes "
                  f"({len(data)} total) - {sent}", flush=True)
            time.sleep(4.0)
            # blank it again
            try:
                blank = bytes([REPORT_ID]) + bytes(data_len)
                if kind == "output":
                    dev.send_output_report(blank)
                else:
                    dev.send_feature_report(blank)
            except Exception:
                pass
            time.sleep(1.5)
    print("\ndone - which candidate lit up?", flush=True)
    dev.close()


if __name__ == "__main__":
    main()

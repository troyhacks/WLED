#!/usr/bin/env python3
"""Replay captured HID reports through a HIDCTL profile.

Reads the profile out of the firmware header
(usermods/usermod_v3_hidctl/hidctl_default_profile.h), decodes a capture
made with tools/f1_hid_probe.py, and prints the actions the usermod would
fire. It is the same decode the ESP32-P4 does, so a profile that passes
here behaves the same on the board.

Usage:
    python tools/f1_profile_check.py tools/f1_reports_final.log
"""
import json
import re
import sys

HEADER = "usermods/usermod_v3_hidctl/hidctl_default_profile.h"


def load_profile(path=HEADER):
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    m = re.search(r'R"JSON\((.*?)\)JSON"', text, re.S)
    if not m:
        raise SystemExit(f"no profile JSON found in {path}")
    return json.loads(m.group(1))


def load_reports(path):
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


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    log = args[0] if args else "tools/f1_reports_final.log"
    prof = load_profile()

    print(f"profile : {prof['name']} {prof['vid']}:{prof['pid']}")
    print(f"report  : id={prof.get('report_id')} len={prof['report_len']}")
    print(f"buttons : bytes={prof['buttons']['bytes']} "
          f"msb_first={prof['buttons'].get('msb_first', True)}")
    print(f"controls: {len(prof['controls'])}  buttons mapped: {len(prof['button_map'])}\n")

    reports = load_reports(log)
    if not reports:
        sys.exit(f"no reports in {log}")

    # --- build the lookup tables the same way the firmware does ---
    btn_bytes = prof["buttons"]["bytes"]
    msb_first = prof["buttons"].get("msb_first", True)

    button_actions = {b["index"]: (b.get("action", ""), b.get("mod_action", ""),
                                  b.get("modifier", False)) for b in prof["button_map"]}
    modifiers = 0
    for i, (_, _, m) in button_actions.items():
        if m:
            modifiers |= 1 << i

    prev_values = {}
    prev_buttons = 0
    events = []
    for t, data in reports:
        if len(data) < prof["report_len"]:
            continue
        if prof.get("report_id") and data[0] != prof["report_id"]:
            continue

        # continuous controls
        for c in prof["controls"]:
            off, bits = c["offset"], c["bits"]
            raw = 0
            for b in range((bits + 7) // 8):
                raw |= data[off + b] << (8 * b)
            raw &= (1 << bits) - 1
            if c["name"] not in prev_values:
                prev_values[c["name"]] = raw
                continue
            if prev_values[c["name"]] == raw:
                continue
            prev_values[c["name"]] = raw
            full = (1 << bits) - 1
            val = raw * 255 // full
            events.append((t, f"{c['name']:8s} = {raw:5d} -> {c.get('action','') or '(none)'} = {val}"))

        # buttons
        now = 0
        for bi, b in enumerate(btn_bytes):
            byte = data[b]
            for bit in range(8):
                set_ = bool(byte & (0x80 >> bit)) if msb_first else bool(byte & (1 << bit))
                if set_:
                    now |= 1 << (bi * 8 + bit)
        pressed = now & ~prev_buttons
        prev_buttons = now
        mod_held = bool(now & modifiers)
        for idx, (act, mod_act, _) in button_actions.items():
            if not (pressed & (1 << idx)):
                continue
            if act is None:
                continue
            chosen = mod_act if (mod_held and mod_act) else act
            name = next((b["name"] for b in prof["button_map"] if b["index"] == idx), f"btn{idx}")
            events.append((t, f"{name:8s} PRESS -> {chosen or '(none)'}"))

    print(f"{len(reports)} reports decoded, {len(events)} events\n")
    # Collapse runs of the same event kind to keep the output readable.
    last = None
    count = 0
    for t, line in events:
        key = re.sub(r"= *\d+", "= N", line)
        if key == last:
            count += 1
            continue
        if last is not None:
            print(f"    ... x{count}")
        print(f"{t:9.3f}s  {line}")
        last, count = key, 1
    if last is not None:
        print(f"    ... x{count}")


if __name__ == "__main__":
    main()

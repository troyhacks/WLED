#!/usr/bin/env python3
"""Watch the Traktor Kontrol F1's MIDI port and decode what it sends.

The F1 is a HID device; NI's driver turns it into a MIDI endpoint
("Traktor Kontrol F1 - 1 0" for input). This tool prints every message
with the usual shorthand so the control map can be written down.

Usage:
    python tools/f1_midi_probe.py --seconds 30
    python tools/f1_midi_probe.py --seconds 60 --out tools/f1_midi.log
"""
import argparse
import sys
import time

try:
    import rtmidi
except ImportError:
    sys.exit("rtmidi not installed: python -m pip install python-rtmidi")

# Common CC numbers, so the log is readable without a MIDI monitor.
CC_NAMES = {
    1: "mod", 2: "breath", 7: "vol", 10: "pan", 11: "expr",
    64: "sustain",
    71: "filter", 74: "resonance", 91: "reverb", 93: "chorus",
}

NOTE_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")


def note_name(n):
    return f"{NOTE_NAMES[n % 12]}{n // 12 - 1}"


def decode(data):
    """Turn a raw MIDI byte list into a readable string."""
    if not data:
        return "(empty)"
    status = data[0]
    chan = status & 0x0F
    kind = status & 0xF0

    if status == 0xF0:
        return f"SysEx len={len(data)} {bytes(data).hex(' ')}"
    if status in (0xF1, 0xF3):
        return f"System {status:#04x} {bytes(data[1:]).hex(' ')}"
    if status == 0xF2:
        return f"Position {bytes(data[1:]).hex(' ')}"
    if status in (0xF6, 0xF8, 0xFE):
        return f"Realtime {status:#04x}"
    if status in (0xF4, 0xF5, 0xF7, 0xF9, 0xFD):
        return f"Undefined {status:#04x}"

    if kind == 0x90:
        note, vel = data[1], data[2]
        return (f"NoteOn  ch{chan + 1} note={note} ({note_name(note)}) vel={vel}"
                if vel else f"NoteOff ch{chan + 1} note={note} ({note_name(note)})")
    if kind == 0x80:
        return f"NoteOff ch{chan + 1} note={data[1]} ({note_name(data[1])})"
    if kind == 0xB0:
        num, val = data[1], data[2]
        name = CC_NAMES.get(num, "")
        extra = f" ({name})" if name else ""
        return f"CC     ch{chan + 1} cc={num}{extra} value={val}"
    if kind == 0xA0:
        return f"PolyKey ch{chan + 1} note={data[1]} press={data[2]}"
    if kind == 0xC0:
        return f"Program ch{chan + 1} program={data[1]}"
    if kind == 0xD0:
        return f"ChanPress ch{chan + 1} value={data[1]}"
    if kind == 0xE0:
        bend = (data[2] << 7) | data[1]
        return f"PitchBend ch{chan + 1} value={bend - 8192}"
    return f"raw {bytes(data).hex(' ')}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--port", default="Kontrol F1",
                    help="substring of the input port name")
    ap.add_argument("--out", help="append decoded messages to this file")
    args = ap.parse_args()

    midi_in = rtmidi.MidiIn()
    ports = midi_in.get_ports()
    chosen = None
    for i, p in enumerate(ports):
        label = p if isinstance(p, str) else p["name"]
        print(f"port {i}: {label}")
        if args.port.lower() in label.lower():
            chosen = (i, label)
    if chosen is None:
        sys.exit(f"no input port matching {args.port!r}")

    idx, label = chosen
    print(f"\nopening {label} (port {idx})")
    midi_in.open_port(idx)
    midi_in.ignore_types(sysex=True, timing=True, active_sense=True)

    print(f"listening for {args.seconds}s -- turn knobs, press buttons\n")
    t0 = time.time()
    fh = open(args.out, "a", encoding="utf-8") if args.out else None
    seen = {}
    try:
        while time.time() - t0 < args.seconds:
            msg = midi_in.get_message()
            if msg is None:
                time.sleep(0.005)
                continue
            t, raw = msg
            text = decode(list(raw))
            line = f"{t - t0:8.3f}s {text}"
            print(line)
            if fh:
                fh.write(line + "\n")
                fh.flush()
            key = text.split(" ch")[0]
            seen[key] = seen.get(key, 0) + 1
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        if fh:
            fh.close()
        midi_in.close_port()

    print("\n=== summary ===")
    for k, v in sorted(seen.items(), key=lambda kv: -kv[1]):
        print(f"  {v:5d}  {k}")


if __name__ == "__main__":
    main()

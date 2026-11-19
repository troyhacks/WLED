"""
Watch the deck's COUNT - the song's beat number - and whether it resets.

The STATUS packet carries:
    0xA0..0xA3  the beats elapsed  (the song's beat count)
    0x2C..0x2F  the track id        (changes when the track changes)
    0x14C..0x14F the track duration

The question this answers: when a NEW TRACK starts, does the count go
back to zero, or does it carry on from where the last track left it?

Usage:
    python tools/deck_count.py [seconds]
"""
import socket
import struct
import sys
import time


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 50002))
    s.settimeout(0.2)

    rows = []
    t0 = time.time()
    while time.time() - t0 < seconds:
        try:
            data, _ = s.recvfrom(2048)
        except socket.timeout:
            continue
        t = time.time() - t0
        if len(data) < 0xA7:
            continue
        elapsed = struct.unpack(">I", data[0xA0:0xA4])[0]
        inbar = data[0xA6]
        tid = struct.unpack(">I", data[0x2C:0x30])[0] if len(data) >= 0x30 else -1
        dur = struct.unpack(">I", data[0x14C:0x150])[0] if len(data) >= 0x150 else -1
        rows.append((t, data[0x21], inbar, elapsed, tid, dur))

    print(f"--- {seconds:.0f} s of STATUS packets ---")
    print("     t   player  bar  beats_elapsed  track_id  duration_s")
    for r in rows:
        print("  %6.3f   %5d   %d      %8d     %8d   %9.1f"
              % (r[0], r[1], r[2], r[3], r[4], r[5] / 1000.0))

    if rows:
        print("\nfirst count %d, last count %d, track id %s"
              % (rows[0][3], rows[-1][3], rows[-1][4]))
        if len({r[4] for r in rows}) > 1:
            print("THE TRACK CHANGED during the capture - the counts around it:")
            for i, r in enumerate(rows):
                if i and r[4] != rows[i - 1][4]:
                    print("  before: count %d (track %d)" % (rows[i - 1][3], rows[i - 1][4]))
                    print("  after:  count %d (track %d)" % (r[3], r[4]))


if __name__ == "__main__":
    main()

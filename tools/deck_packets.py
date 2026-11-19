"""
Listen to the deck's own packets and see what it actually says.

The board interprets the deck's packets; this is the ground truth to check
that interpretation against.  It listens on the three ProLink ports:

    50000  ANNOUNCE
    50001  BEAT      0x54 speed, 0x5A bpm x100, 0x5C the beat number
    50002  STATUS    0x99 pitch, 0xA0 the beats elapsed, 0xA6 the beat in the bar

Usage:
    python tools/deck_packets.py [seconds]
"""
import socket
import struct
import sys
import time

PORTS = {50000: "ANNOUNCE", 50001: "BEAT", 50002: "STATUS"}


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 6.0
    socks = {}
    for port in PORTS:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("0.0.0.0", port))
        s.settimeout(0.2)
        socks[port] = s

    counts = {p: 0 for p in PORTS}
    beats, statuses = [], []
    t0 = time.time()
    while time.time() - t0 < seconds:
        for port, s in socks.items():
            try:
                data, addr = s.recvfrom(2048)
            except socket.timeout:
                continue
            counts[port] += 1
            t = time.time() - t0
            if port == 50001 and len(data) >= 0x5D:
                bpm = struct.unpack(">H", data[0x5A:0x5C])[0] / 100.0
                beats.append((t, data[0x21], data[0x5C], bpm))
            elif port == 50002 and len(data) >= 0xA7:
                elapsed = struct.unpack(">I", data[0xA0:0xA4])[0]
                statuses.append((t, data[0x21], data[0xA6], elapsed))
        time.sleep(0.002)

    print(f"--- {seconds:.0f} s ---")
    for port, name in PORTS.items():
        print(f"  {name:9} port {port}: {counts[port]} packets "
              f"({counts[port]/seconds:.1f}/s)")

    print("\n  the BEAT packets (t, player, beat number, bpm):")
    for b in beats[:24]:
        print("    %6.3f  player %d  beat %d  %6.2f BPM" % b)
    print("\n  the STATUS packets (t, player, beat in bar, beats elapsed):")
    for s in statuses[:24]:
        print("    %6.3f  player %d  beat in bar %d  elapsed %d" % s)


if __name__ == "__main__":
    main()

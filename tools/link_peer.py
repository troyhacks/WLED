"""
link_peer.py - try to become a Link peer, so Live tells us its PHASE.

The tempo comes out of the announce traffic for free (it is a property of
the session), but the PHASE - where in the bar we are - is only shared
between peers.  So we have to announce ourselves as one.

The announce packet is:

    "_asdp_v"  version=1 type=1 flags=5  <12-byte session id>
    then records, each: uint32 length, the value, then the name

and the record that matters is "sess" (24 bytes), which holds the tempo
as MICROSECONDS PER BEAT at +4, with counters either side.

The first attempt at this sent an all-zero "sess" and Live ignored it.
This one sends a real tempo and real counters.

    python tools/link_peer.py --seconds 20
"""

import argparse
import os
import socket
import struct
import sys
import time

GROUP = "224.76.78.75"
PORT = 20808


def build_announce(session_id, peer_id, tempo_us=500000, counter=0, phase=0):
    p = bytearray()
    p += b"_asdp_v"
    p += bytes([1, 1, 5, 0, 0])
    p += session_id
    # the "sess" record: 24 bytes, tempo in microseconds per beat at +4
    sess = struct.pack(">I", 0) + struct.pack(">I", tempo_us) + struct.pack(">I", 0)
    sess += struct.pack(">I", counter) + struct.pack(">I", 0) + struct.pack(">I", phase)
    p += struct.pack(">I", len(sess)) + sess + b"sess"
    p += struct.pack(">I", len(peer_id)) + peer_id + b"stst"
    p += struct.pack(">I", 17) + bytes(17) + b"mep4"
    p += struct.pack(">I", 6) + os.urandom(6)
    return bytes(p)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--iface", default=None)
    ap.add_argument("--tempo", type=int, default=500000, help="microseconds per beat")
    args = ap.parse_args()

    iface = args.iface
    if not iface:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.168.6.131", 9))
        iface = s.getsockname()[0]
        s.close()

    # listen
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    rx.bind(("", PORT))
    rx.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                  struct.pack("4s4s", socket.inet_aton(GROUP), socket.inet_aton(iface)))
    rx.settimeout(0.2)

    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tx.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    tx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)

    sid = os.urandom(12)
    pid = os.urandom(8)
    print("our session id %s" % sid.hex())
    print("our peer id    %s" % pid.hex())
    print("announcing on %s:%d every 0.5 s, tempo %d us/beat (%.4f BPM)\n"
          % (GROUP, PORT, args.tempo, 60000000.0 / args.tempo))

    seen = {}
    interesting = []
    counter = 0
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        counter += 1
        pkt = build_announce(sid, pid, args.tempo, counter * 2000000, counter * 1000000)
        tx.sendto(pkt, (GROUP, PORT))
        time.sleep(0.5)
        # drain
        for _ in range(30):
            try:
                d, a = rx.recvfrom(4096)
            except socket.timeout:
                break
            key = (d[:7], len(d))
            seen[key] = seen.get(key, 0) + 1
            # is it ours coming back, or something new?
            if d[12:24] != sid:
                interesting.append((time.monotonic(), a, d))
    print("what we heard:")
    for (magic, ln), c in sorted(seen.items(), key=lambda kv: -kv[1]):
        print("  %r  %d bytes  %d packets%s"
              % (magic, ln, c, "   <-- OURS" if c else ""))
    if interesting:
        print("\n*** %d packets that were NOT ours - these are the answer ***" % len(interesting))
        for t, a, d in interesting[:4]:
            print("  from %s  %d bytes" % (a[0], len(d)))
            print("    %s" % " ".join("%02x" % b for b in d[:64]))
            print("    %s" % "".join(chr(b) if 32 <= b < 127 else "." for b in d[:64]))
    else:
        print("\nnothing new - Live still is not talking to us.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

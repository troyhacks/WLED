"""
link_decode.py - read Ableton Link's clock off the wire.

WHAT WAS FOUND, by watching a PRIME GO... no: by watching Ableton Live 12
Lite on the same wire, with the tempo known (120 BPM):

    UDP multicast 224.76.78.75
      port 20808   the ASDP announce channel, ~20 packets a second
      port 20909   the ablsd start/stop channel, one a second

An ASDP packet is:

    "_asdp_v"            7 bytes of magic
    01 01 05 00 00       version, message type, and a count
    <12 bytes>           the session id
    <records>            each one: a uint32 length, the value, then the name

The record that matters is "sess" (24 bytes), and inside it:

    +0   uint32   0
    +4   uint32   MICROSECONDS PER BEAT   ->  60 000 000 / this = the tempo
    +8   uint32   0
    +12  uint32   a counter that marches every packet
    +16  uint32   0
    +20  uint32   a counter that marches every packet

At 120 BPM the tempo field reads 500 000, which is exactly one beat at
120 BPM - and it is the field that moves when Live's tempo moves.

    python tools/link_decode.py --seconds 10
"""

import argparse
import socket
import struct
import sys
import time

GROUP = "224.76.78.75"
PORT = 20808


def parse_asdp(data):
    """Pull the records out of an ASDP packet."""
    if len(data) < 12 or data[:7] != b"_asdp_v":
        return None
    out = {"magic": data[:7].decode(), "version": data[7], "type": data[8],
           "count": data[9] if len(data) > 9 else 0,
           "session": data[12:24].hex(), "records": {}}
    o = 24
    while o + 4 <= len(data):
        n = struct.unpack_from(">I", data, o)[0]
        o += 4
        if n > len(data) - o:
            break
        val = data[o:o + n]
        o += n
        # the name follows the value
        name = ""
        while o < len(data) and 32 <= data[o] < 127:
            name += chr(data[o])
            o += 1
        out["records"][name or ("@%d" % o)] = val
    return out


def tempo_bpm(us_per_beat):
    if not us_per_beat:
        return 0.0
    return 60000000.0 / float(us_per_beat)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--iface", default=None)
    args = ap.parse_args()

    iface = args.iface
    if not iface:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.168.6.131", 9))
        iface = s.getsockname()[0]
        s.close()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", PORT))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                 struct.pack("4s4s", socket.inet_aton(GROUP), socket.inet_aton(iface)))
    s.settimeout(0.3)
    print("Ableton Link: %s:%d on %s\n" % (GROUP, PORT, iface))

    packets = []
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        try:
            d, _a = s.recvfrom(4096)
        except socket.timeout:
            continue
        packets.append((time.monotonic(), d))

    if not packets:
        print("nothing arrived - is Link on?")
        return 1

    p = packets[0][1]
    info = parse_asdp(p)
    if not info:
        print("not an ASDP packet")
        return 1

    print("%d packets.  The first one:" % len(packets))
    print("  magic    %r" % info["magic"])
    print("  version  %d   type %d   flags %d" % (info["version"], info["type"], info["count"]))
    print("  session  %s" % info["session"])
    for name, val in info["records"].items():
        print("  record %-6s %2d bytes  %s" % (name, len(val), val.hex()))
        if name == "sess" and len(val) >= 24:
            us = struct.unpack_from(">I", val, 4)[0]
            print("           -> microseconds per beat = %d   ->  TEMPO = %.4f BPM"
                  % (us, tempo_bpm(us)))
            print("           -> counters at +12 and +20 march every packet")
    print()

    # the tempo over the capture
    bpm = {}
    for _t, d in packets:
        i = parse_asdp(d)
        if i and "sess" in i["records"] and len(i["records"]["sess"]) >= 24:
            us = struct.unpack_from(">I", i["records"]["sess"], 4)[0]
            bpm[round(tempo_bpm(us), 4)] = bpm.get(round(tempo_bpm(us), 4), 0) + 1
    if bpm:
        print("the tempo over %d packets: %s"
              % (len(packets), ", ".join("%.4f BPM (x%d)" % (k, v) for k, v in bpm.items())))

    # the rate of the marching counter, in beats per second
    c12 = []
    for t, d in packets:
        i = parse_asdp(d)
        if i and "sess" in i["records"] and len(i["records"]["sess"]) >= 24:
            c12.append((t, struct.unpack_from(">I", i["records"]["sess"], 12)[0]))
    if len(c12) > 3:
        dt = c12[-1][0] - c12[0][0]
        du = (c12[-1][1] - c12[0][1]) & 0xFFFFFFFF
        us = None
        i = parse_asdp(packets[0][1])
        if i and "sess" in i["records"]:
            us = struct.unpack_from(">I", i["records"]["sess"], 4)[0]
        if dt > 0:
            rate = du / dt
            print("counter +12 advances %.0f per second" % rate)
            if us:
                print("           = %.4f beats per second (the tempo is %.4f)"
                      % (rate / us, 60000000.0 / us))
                print("           = %.4f of 2^32 per second" % (rate / 2 ** 32))
    return 0


if __name__ == "__main__":
    sys.exit(main())

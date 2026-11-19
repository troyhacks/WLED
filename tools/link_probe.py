"""
link_probe.py - see whether Ableton Link is on the network, and what it says.

Ableton Link is Ableton's clock sync: apps on the LAN share a tempo and a
beat phase over UDP multicast.  It is the same idea as the Denon deck's
clock, arrived at from the other direction - and if Live is the master,
the board could follow IT.

Link's transport (from the public Link SDK):

    UDP multicast, port 20800
    group 224.0.0.87   the session - who is here, and the tempo
    group 224.0.0.88   the clock - the beat phase

Nothing here needs a special library: a socket, a multicast join, and a
read.  What the packets MEAN is worked out from what arrives.

    python tools/link_probe.py --seconds 10
    python tools/link_probe.py --seconds 30 --decode
"""

import argparse
import socket
import struct
import sys
import time

# These are the values MEASURED on this network, from Ableton Live 12:
#   224.76.78.75:20808   the ASDP announce channel, ~18 packets a second
#   224.76.78.75:20909   the ablsd start/stop channel, one a second
# (the widely-cited 224.0.0.87:20800 is the SDK's own default, not what
#  Live 12 actually uses here)
LINK_PORT = 20808
LINK_GROUPS = ["224.76.78.75"]
LINK_PORTS = [20808, 20909]


def hexdump(data, limit=160):
    out = []
    for off in range(0, min(len(data), limit), 16):
        chunk = data[off:off + 16]
        hexpart = " ".join("%02x" % b for b in chunk)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        out.append("    %04x  %-47s  %s" % (off, hexpart, text))
    if len(data) > limit:
        out.append("    ... %d more bytes" % (len(data) - limit))
    return "\n".join(out)


def try_decode(data):
    """Best effort: what can be read out of this packet."""
    notes = []
    # The Link SDK frames each datagram as a sequence of messages, each one
    # a type byte and a payload.  The session id is 16 bytes and appears in
    # most of them.
    if not data:
        return notes
    notes.append("type byte 0x%02x" % data[0])
    # a tempo is a float64, little endian, and Link's tempo is 4..500 BPM
    for off in range(1, min(len(data) - 8, 64)):
        t = struct.unpack_from("<d", data, off)[0]
        if 20.0 < t < 400.0 and abs(t - round(t)) < 0.01:
            notes.append("float64 at %d = %.6f  (looks like a tempo)" % (off, t))
    # a phase is a numerator/denominator pair of int32
    for off in range(1, min(len(data) - 8, 64)):
        num, den = struct.unpack_from("<ii", data, off)
        if den == 1920 and 0 <= num < 1920:
            notes.append("int32 pair at %d = %d/%d  (looks like a beat phase)"
                         % (off, num, den))
    return notes


def open_sockets(groups, port, iface):
    socks = []
    for g in groups:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        except (AttributeError, OSError):
            pass
        try:
            s.bind(("", port))
        except OSError as e:
            print("  cannot bind %d (%s) - is Link already running?" % (port, e))
            s.close()
            continue
        mreq = struct.pack("4s4s", socket.inet_aton(g), socket.inet_aton(iface))
        try:
            s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
            print("  joined %s on %s" % (g, iface))
        except OSError as e:
            print("  could not join %s: %s" % (g, e))
        socks.append((g, s))
    return socks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--port", type=int, default=LINK_PORT)
    ap.add_argument("--ports", nargs="*", type=int, default=LINK_PORTS)
    ap.add_argument("--groups", nargs="*", default=LINK_GROUPS)
    ap.add_argument("--iface", default=None, help="the local IPv4 to join on")
    ap.add_argument("--decode", action="store_true", help="try to read the packets")
    ap.add_argument("--raw", action="store_true", help="dump every packet in hex")
    args = ap.parse_args()

    iface = args.iface
    if not iface:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.168.6.131", 9))
        iface = s.getsockname()[0]
        s.close()

    print("Ableton Link probe: %s:%d, groups %s, on %s"
          % (iface, args.port, ", ".join(args.groups), iface))
    socks = []
    for p in args.ports:
        socks += open_sockets(args.groups, p, iface)
    if not socks:
        print("\nNO SOCKET - Link is not reachable from here.")
        return 1

    for _g, s in socks:
        s.settimeout(0.3)
    socks = [(g, s) for (g, s) in socks]

    print("\nlistening for %.0f s ...\n" % args.seconds)
    end = time.monotonic() + args.seconds
    seen = {}
    count = 0
    while time.monotonic() < end:
        for g, s in socks:
            try:
                data, addr = s.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                continue
            count += 1
            key = (g, addr[0], len(data))
            seen[key] = seen.get(key, 0) + 1
            if args.raw or count <= 6:
                print("%s  from %s:%d  via %s  %d bytes"
                      % (time.strftime("%H:%M:%S"), addr[0], addr[1], g, len(data)))
                print(hexdump(data))
                if args.decode:
                    for n in try_decode(data):
                        print("      %s" % n)
                print()
    print("%d packets" % count)
    if seen:
        print("\n  group/from/size          count")
        for (g, ip, ln), c in sorted(seen.items(), key=lambda kv: -kv[1]):
            print("  %-16s %-16s %4d  %5d" % (g, ip, ln, c))
    if not count:
        print("\nNOTHING ARRIVED.")
        print("  Ableton Live's Link is off - turn it on in Live's Link menu")
        print("  (the 'Link' button in the top bar, or Options > Link), and")
        print("  make sure 'Link On' is enabled.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

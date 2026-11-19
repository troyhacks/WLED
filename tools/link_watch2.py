"""
link_watch2.py - watch every field of the session state, across a restart.

The ASDP payload is a sequence of entries.  Live's carry four:

    key 0x00000018  "sess"  24 bytes - the session state
    key "sess"            8 bytes - half the node id
    key "stst"            8 bytes - half the node id
    key 0x00000011       17 bytes - "mep4"
    key "mep4"             6 bytes - the address and port

The session state's 24 bytes are six uint32s:

    +0   0
    +4   MICROECONDS PER BEAT   ->  the tempo is 60000000 / this
    +8   0
    +12  a counter that marches at 2e6 per second
    +16  something that was 0 before the transport restarted, and 208 after
    +20  a counter that marches at 1e6 per second (microseconds)

And "mep4" was seventeen ZERO bytes before the restart, and is
01 00000000 bed8420c 00000000 5f745972 now - so it has a flag in it, and
that flag appeared when the transport restarted.

So: watch all of it, and watch what a restart does to it.

    python tools/link_watch2.py --seconds 120
"""

import argparse
import socket
import struct
import sys
import time

GROUP = "224.76.78.75"
PORT = 20808


def entries(data):
    """The payload entries.  Each is a length, a value, then a key."""
    out = {}
    o = 24
    while o + 4 <= len(data):
        size = struct.unpack_from(">I", data, o)[0]
        o += 4
        if size > len(data) - o:
            break
        val = data[o:o + size]
        o += size
        key = ""
        while o < len(data) and 32 <= data[o] < 127:
            key += chr(data[o])
            o += 1
        out[key] = val
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=120.0)
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
    s.settimeout(0.05)

    t0 = time.time()
    print("watching %s:%d for %.0f s" % (GROUP, PORT, args.seconds))
    print("%8s %-10s %-10s %-10s %-10s %-6s  %s" %
          ("t", "sess+8", "sess+12", "sess+16", "sess+20", "tempo", "mep4"))
    last = None
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        try:
            d, _a = s.recvfrom(4096)
        except socket.timeout:
            continue
        if len(d) < 24 or d[:7] != b"_asdp_v" or d[7] != 0x01:
            continue
        e = entries(d)
        sess = e.get("sess") or e.get("")
        if not sess or len(sess) < 24:
            continue
        f = struct.unpack_from(">6I", sess, 0)
        mep4 = e.get("mep4", b"")
        key = (f[2], f[3], f[4], f[5], mep4)
        if key != last:
            last = key
            print("%8.2f %-10d %-10d %-10d %-10d %-6d  %s" %
                  (time.time() - t0, f[2], f[3], f[4], f[5], f[1], mep4.hex()),
                  flush=True)
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())

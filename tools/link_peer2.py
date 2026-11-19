"""
link_peer2.py - try harder to become a Link peer.

What the SDK's header tells us (include/ableton/discovery/v1/Messages.hpp):

    "_asdp_v1"            8 bytes of magic (the version is the 8th byte)
    uint8   messageType   1 = alive, 2 = response, 3 = bye-bye
    uint8   ttl
    uint16  groupId       the SESSION GROUP - the session lives in this
    12 bytes               node id
    then the payload: records of  uint32 length, the value, then the name

Live's own announce carries four records: sess (24 bytes - the tempo and
two clocks), stst (8), mep4 (17) and mep (6).

**THE DEFECT IN OUR LAST ATTEMPT:** the "mep" record carries the sender's
OWN address and the port it can be reached on, and we filled it with
random bytes - so the unit had no way to reach us, and had no reason to
answer.  This one puts our real address and a port we are really
listening on.

So: open a listener, announce it, and see what comes back.

    python tools/link_peer2.py --seconds 25
"""

import argparse
import os
import socket
import struct
import sys
import time

GROUP = "224.76.78.75"
PORT = 20808


def parse_asdp(data):
    """The header and the records of an ASDP packet."""
    # The header is "_asdp_v" plus a VERSION BYTE of 0x01 - not the ASCII
    # '1'.  Getting that wrong means we never match a packet and we announce
    # with a magic the unit does not recognise.
    if len(data) < 24 or data[:7] != b"_asdp_v" or data[7] != 0x01:
        return None
    out = {"type": data[8], "ttl": data[9], "group": struct.unpack_from(">H", data, 10)[0],
           "node": data[12:24].hex(), "records": {}}
    o = 24
    while o + 4 <= len(data):
        n = struct.unpack_from(">I", data, o)[0]
        o += 4
        if n > len(data) - o:
            break
        val = data[o:o + n]
        o += n
        name = ""
        while o < len(data) and 32 <= data[o] < 127:
            name += chr(data[o])
            o += 1
        out["records"][name or "?"] = val
    return out


def build_alive(node, group, tempo_us, counter, port):
    """Our own alive message, shaped exactly like Live's."""
    p = bytearray()
    p += b"_asdp_v" + bytes([1])     # the magic and the version byte
    p += bytes([1, 1])              # kAlive, ttl 1
    p += struct.pack(">H", group)
    p += node[:12].ljust(12, b"\x00")
    # sess: 24 bytes, the tempo in microseconds per beat at +4
    sess = (struct.pack(">I", 0) + struct.pack(">I", tempo_us) + struct.pack(">I", 0)
            + struct.pack(">I", counter) + struct.pack(">I", 0) + struct.pack(">I", counter))
    p += struct.pack(">I", len(sess)) + sess + b"sess"
    # stst: 8 bytes - Live puts the first 8 bytes of its node id here
    p += struct.pack(">I", 8) + node[:8] + b"stst"
    # mep4: 17 bytes of nothing
    p += struct.pack(">I", 17) + bytes(17) + b"mep4"
    # mep: 6 bytes - our IPv4 and the port we are listening on
    ip = socket.inet_aton(socket.gethostbyname(socket.gethostname()))
    p += struct.pack(">I", 6) + ip + struct.pack(">H", port)
    return bytes(p)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=25.0)
    ap.add_argument("--iface", default=None)
    args = ap.parse_args()

    iface = args.iface
    if not iface:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.168.6.131", 9))
        iface = s.getsockname()[0]
        s.close()

    # a listener the unit can actually reach us on
    lis = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    lis.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    lis.bind(("0.0.0.0", 0))
    lis.listen(4)
    lis.settimeout(0.2)
    lport = lis.getsockname()[1]
    print("listening on port %d for the unit's call back" % lport)

    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    rx.bind(("", PORT))
    rx.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                  struct.pack("4s4s", socket.inet_aton(GROUP), socket.inet_aton(iface)))
    rx.settimeout(0.05)

    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tx.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)

    node = os.urandom(12)
    print("our node id %s" % node.hex())

    # what does Live say about ITSELF?
    live = None
    end = time.monotonic() + 3
    while time.monotonic() < end and live is None:
        try:
            d, _a = rx.recvfrom(4096)
        except socket.timeout:
            continue
        info = parse_asdp(d)
        if info and info["type"] == 1 and info["node"] != node.hex():
            live = info
    if live:
        print("\nLive's announce:")
        print("  group %d  node %s" % (live["group"], live["node"]))
        for name, val in live["records"].items():
            print("  %-6s %2d bytes  %s" % (name, len(val), val.hex()))
        mep = live["records"].get("mep")
        if mep and len(mep) == 6:
            ip = socket.inet_ntoa(mep[:4])
            port = struct.unpack(">H", mep[4:])[0]
            print("  -> its peer address: %s:%d" % (ip, port))
            try:
                c = socket.create_connection((ip, port), timeout=2)
                print("  -> TCP connect to it: OK")
                c.settimeout(2)
                try:
                    buf = c.recv(4096)
                except socket.timeout:
                    buf = b""
                print("  -> it sent %d bytes: %s" % (len(buf), buf[:64].hex()))
                c.close()
            except OSError as e:
                print("  -> TCP connect to it: %s" % e)
    else:
        print("\nno Live announce seen")

    # now announce ourselves, properly
    group = live["group"] if live else 5
    tempo = 500000
    print("\nannouncing ourselves (group %d, tempo %d us/beat, our port %d)..."
          % (group, tempo, lport))
    seen = {}
    t0 = time.monotonic()
    ctr = 0
    while time.monotonic() - t0 < args.seconds:
        ctr += 1
        pkt = build_alive(node, group, tempo, ctr * 1000000, lport)
        tx.sendto(pkt, (GROUP, PORT))
        tx.sendto(pkt, (socket.inet_ntoa(socket.inet_aton(socket.gethostbyname(
            socket.gethostname()))), PORT))
        time.sleep(0.25)
        # a call back on our listener?
        try:
            c, addr = lis.accept()
            c.settimeout(2)
            buf = b""
            try:
                while len(buf) < 512:
                    x = c.recv(4096)
                    if not x:
                        break
                    buf += x
            except socket.timeout:
                pass
            print("  *** THE UNIT CALLED BACK from %s:%d, %d bytes ***" % (addr[0], addr[1], len(buf)))
            print("      %s" % buf[:96].hex())
            if len(buf) >= 24 and buf[:7] == b"_asdp_v" and buf[7] == 0x01:
                info = parse_asdp(buf)
                print("      type=%d group=%d node=%s" % (info["type"], info["group"], info["node"]))
                for name, val in info["records"].items():
                    print("      %-6s %2d bytes  %s" % (name, len(val), val.hex()))
                    if name == "sess" and len(val) >= 24:
                        print("           tempo = %d us/beat -> %.4f BPM"
                              % (struct.unpack_from(">I", val, 4)[0],
                                 60000000.0 / struct.unpack_from(">I", val, 4)[0]))
            c.close()
        except socket.timeout:
            pass
        # anything new on the wire?
        for _ in range(20):
            try:
                d, a = rx.recvfrom(4096)
            except socket.timeout:
                break
            info = parse_asdp(d)
            if info is None:
                continue
            key = (info["type"], info["node"], tuple(sorted(info["records"].keys())))
            if key not in seen:
                seen[key] = (a, info)
                if info["node"] != node.hex():
                    print("  new traffic: type=%d group=%d from %s:%d records=%s"
                          % (info["type"], info["group"], a[0], a[1], list(info["records"].keys())))
    print("\ndone")
    return 0


if __name__ == "__main__":
    sys.exit(main())

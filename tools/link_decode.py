"""
link_decode.py - read Ableton Link's tempo and phase off the wire.

WHAT WAS FOUND, by watching Ableton Live 12 Lite on the same wire with the
tempo known (120 BPM):

    UDP multicast 224.76.78.75
      :20808   the ASDP announce channel, ~18 packets a second
      :20909   the ablsd start/stop channel, one a second

(The widely-cited 224.0.0.87:20800 is the Link SDK's own default and is
NOT what Live 12 uses here.  Find the real group with
`netsh interface ipv4 show joins`.)

An ASDP packet:

    "_asdp_v" + a version byte of 0x01     8 bytes
    uint8   message type                    1 = alive, 2 = response
    uint8   ttl
    uint16  group id                        the session group
    12 bytes                                the node id
    then the payload, as entries of:
        uint32  length
        <value>
        uint32  key   - the key is FOUR ASCII CHARACTERS, big endian

Live carries four entries.  The one that matters is "sess", and it is the
SDK's Timeline - (Tempo, Beats, timeOrigin), 24 bytes, each an int64 in
big endian:

    +0   int64   MICROSECONDS PER BEAT      the tempo is 60000000 / this
    +8   int64   MILLIONTHS OF A BEAT       the phase, and it advances at
                                              the beat rate - 2 000 000
                                              millionths a second at 120 BPM
    +16  int64   a microsecond time origin

So the phase comes straight out of the announce traffic: no peer handshake
is needed to follow Link's clock.

    python tools/link_decode.py --seconds 10
"""

import argparse
import socket
import struct
import sys
import time

GROUP = "224.76.78.75"
PORT = 20808


def entries(data):
    """The payload entries: a length, a value, then a four-character key."""
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
        if o + 4 <= len(data):
            key = "".join(chr(b) if 32 <= b < 127 else "." for b in data[o:o + 4])
            o += 4
        out[key] = val
    return out


def read_timeline(data):
    """(tempo in us per beat, phase in beats, the raw fields)."""
    e = entries(data)
    sess = e.get("sess")
    if not sess or len(sess) < 24:
        return None
    tempo_us, origin, time_origin = struct.unpack_from(">qqq", sess, 0)
    return tempo_us, origin / 1e6, time_origin, sess


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
    s.settimeout(0.05)

    t0 = time.time()
    print("Ableton Link: %s:%d on %s\n" % (GROUP, PORT, iface))
    print("%9s %10s %14s %9s %8s %8s  %s" %
          ("t", "tempo", "phase(beats)", "bar", "beat", "32nd", "track"))
    last = None
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        try:
            d, _a = s.recvfrom(4096)
        except socket.timeout:
            continue
        if len(d) < 48 or d[:7] != b"_asdp_v" or d[7] != 0x01:
            continue
        t = read_timeline(d)
        if not t:
            continue
        tempo_us, phase, time_origin, _sess = t
        bar = int(phase) % 4
        beat = bar + 1
        if (tempo_us, phase) != last:
            last = (tempo_us, phase)
            print("%9.2f %10d %14.6f %9d %8d %8d" %
                  (time.time() - t0, tempo_us, phase, bar, beat,
                   (int(phase * 4) % 32)), flush=True)
    print("\n  the phase advances at the beat rate, so the bar and the beat in")
    print("  the bar fall out of it directly.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

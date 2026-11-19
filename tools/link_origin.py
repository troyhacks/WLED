"""
link_origin.py - find Ableton Link's PHASE, i.e. where in the bar we are.

The tempo comes out of the announce traffic for free: the "sess" record's
u32 at +4 is microseconds per beat, and the tempo is 60000000 / that.

The phase is the beat's position - which beat of the bar we are on - and
that is what a follower needs.  From the announce traffic we have a clock
(the "sess" record's counter at +20, which advances at 1e6 per second, a
microsecond clock), so

    phase = ((clock + K) mod microseconds_per_beat) / microseconds_per_beat

marches at exactly the right rate for ANY K.  What is missing is K: the
anchor, the moment the transport started.

So the point of this script is to watch the transport being stopped and
started, which is when the anchor moves.  It logs the clock, the phase
and the start/stop message, and prints the clock's value at every
change, so the anchor can be read off.

    python tools/link_origin.py --seconds 180
"""

import argparse
import socket
import struct
import sys
import time

GROUP = "224.76.78.75"
ANNOUNCE_PORT = 20808
STARTSTOP_PORT = 20909


def open_socket(port, iface):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", port))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                 struct.pack("4s4s", socket.inet_aton(GROUP), socket.inet_aton(iface)))
    s.settimeout(0.05)
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=180.0)
    ap.add_argument("--iface", default=None)
    args = ap.parse_args()

    iface = args.iface
    if not iface:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.168.6.131", 9))
        iface = s.getsockname()[0]
        s.close()

    ann = open_socket(ANNOUNCE_PORT, iface)
    sst = open_socket(STARTSTOP_PORT, iface)
    t0 = time.time()
    print("watching %s for %.0f s - STOP then PLAY in Live" % (GROUP, args.seconds))
    print("%9s %12s %12s %8s %10s  %s" %
          ("t", "clock", "counter", "tempo", "phase", "start/stop"))
    last = None
    last_sst = None
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        try:
            d, _a = ann.recvfrom(4096)
        except socket.timeout:
            d = None
        if d and len(d) >= 0x34:
            tempo = struct.unpack_from(">I", d, 0x20)[0]
            ctr = struct.unpack_from(">I", d, 0x30)[0]
            if (tempo, ctr) != last:
                last = (tempo, ctr)
                phase = (ctr % tempo) / float(tempo)
                print("%9.2f %12d %12d %8d %10.6f" %
                      (time.time() - t0, ctr, ctr, tempo, phase), flush=True)
        try:
            d2, _a2 = sst.recvfrom(4096)
        except socket.timeout:
            d2 = None
        if d2:
            body = d2[9:]
            if body != last_sst:
                last_sst = body
                print("%9.2f %12s %12s %8s %10s  START/STOP %s" %
                      (time.time() - t0, "-", "-", "-", "-",
                       " ".join("%02x" % b for b in body)), flush=True)
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
stagelinq_probe.py - a host-side StageLinq client for the Denon DJ PRIME GO.

StageLinq is Denon DJ's network protocol for lighting/video control.  A
device broadcasts itself on UDP 51337 and then offers a set of TCP services
(StateMap, BeatInfo, FileTransfer, TimeSynchronization, Broadcast) on
dynamic ports.

This probe is a SINK: it discovers a device, negotiates the services and
subscribes to BeatInfo and/or StateMap.  It exists to answer four questions
before any firmware is written:

  1. does the PRIME GO actually EMIT the BeatInfo beat stream, or only
     advertise the service?
  2. is BeatInfo's 'beat' grid-relative (beat 1 = the downbeat) or
     track-relative?
  3. which token does the device expect in the service request - the
     device's own, or one we make up?
  4. what is the real beat latency (arrival time vs. the beat's own phase)?

Standard library only.  Nothing to install.

    python tools/stagelinq_probe.py discover
    python tools/stagelinq_probe.py capture
    python tools/stagelinq_probe.py beat
    python tools/stagelinq_probe.py state
    python tools/stagelinq_probe.py all --ip 192.168.6.131

The wire format is documented in tools/stagelinq_probe.md next to this file.
"""

import argparse
import binascii
import datetime
import json
import math
import os
import socket
import struct
import sys
import time

# ---------------------------------------------------------------- constants

DISCOVERY_PORT = 51337
DISCOVERY_MAGIC = b"airD"

# main-connection message ids
MSG_SERVICE_ANNOUNCEMENT = 0
MSG_TIMESTAMP = 1
MSG_SERVICES_REQUEST = 2

# BeatInfo service message ids (the "magic" after the length prefix)
BEAT_START = 0x00000000
BEAT_STOP = 0x00000001
BEAT_EMIT = 0x00000002

# StateMap
SMAA_MAGIC = b"smaa"
SMAA_EMIT = 0x00000000
SMAA_SUBSCRIBE = 0x000007D2

DEFAULT_IP = "192.168.6.131"

# The StateMap paths we care about.  Deck 1 and Deck 2 only - the PRIME GO
# has two decks.
STATE_PATHS = [
    "/Engine/Deck1/Play",
    "/Engine/Deck1/PlayState",
    "/Engine/Deck1/CurrentBPM",
    "/Engine/Deck1/MetaDataBPM",
    "/Engine/Deck1/Speed",
    "/Engine/Deck1/SpeedRange",
    "/Engine/Deck1/SpeedState",
    "/Engine/Deck1/SyncMode",
    "/Engine/Deck1/DeckIsMaster",
    "/Engine/Deck1/LoopEnableState",
    "/Engine/Deck1/CurrentLoopInPosition",
    "/Engine/Deck1/CurrentLoopOutPosition",
    "/Engine/Deck1/CurrentLoopSizeInBeats",
    "/Engine/Deck1/CuePosition",
    "/Engine/Deck1/HotCue1",
    "/Engine/Deck1/HotCue2",
    "/Engine/Deck1/PlayPosition",
    "/Engine/Deck1/SongPosition",
    "/Engine/Deck1/TrackPosition",
    "/Engine/Deck1/PlayheadPosition",
    "/Engine/Deck1/Track/SongName",
    "/Engine/Deck1/Track/ArtistName",
    "/Engine/Deck1/Track/TrackName",
    "/Engine/Deck1/Track/TrackLength",
    "/Engine/Deck1/Track/SampleRate",
    "/Engine/Deck1/Track/CuePosition",
    "/Engine/Deck1/Track/CurrentKey",
    "/Engine/Deck1/Track/Genre",
    "/Engine/Deck2/Play",
    "/Engine/Deck2/PlayState",
    "/Engine/Deck2/CurrentBPM",
    "/Engine/Deck2/MetaDataBPM",
    "/Engine/Deck2/Speed",
    "/Engine/Deck2/SyncMode",
    "/Engine/Deck2/DeckIsMaster",
    "/Engine/Deck2/LoopEnableState",
    "/Engine/Deck2/CurrentLoopSizeInBeats",
    "/Engine/Deck2/CuePosition",
    "/Engine/Deck2/Track/SongName",
    "/Engine/Deck2/Track/ArtistName",
    "/Engine/Deck2/Track/TrackLength",
    "/Engine/Master/MasterTempo",
    "/Engine/DeckCount",
    "/GUI/Decks/Deck/ActiveDeck",
]


# ------------------------------------------------------------------ helpers

def hexdump(data, limit=512):
    out = []
    for off in range(0, min(len(data), limit), 16):
        chunk = data[off:off + 16]
        hexpart = " ".join("%02x" % b for b in chunk)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        out.append("  %04x  %-47s  %s" % (off, hexpart, text))
    if len(data) > limit:
        out.append("  ... %d more bytes" % (len(data) - limit))
    return "\n".join(out)


def now():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]


def enc_string(s):
    """A network string: uint32 length IN BYTES, then UTF-16BE."""
    b = s.encode("utf-16-be")
    return struct.pack(">I", len(b)) + b


class Reader:
    """A buffered reader over a socket."""

    def __init__(self, sock):
        self.sock = sock
        self.seg = []          # list of (bytes, recv_time)
        self.eof = False
        self.last_take = 0.0   # monotonic time of the bytes last taken

    def _avail(self):
        return sum(len(s[0]) for s in self.seg)

    def _fill(self, n, timeout):
        if self._avail() >= n:
            return True
        self.sock.settimeout(timeout)
        while self._avail() < n:
            if self.eof:
                return False
            try:
                d = self.sock.recv(4096)
            except socket.timeout:
                return False
            except OSError:
                self.eof = True
                return False
            if not d:
                self.eof = True
                return False
            self.seg.append((d, time.monotonic()))
        return True

    def avail(self):
        """How many bytes are buffered right now."""
        return sum(len(s[0]) for s in self.seg)

    def peek(self, n):
        """The next n buffered bytes, without consuming them."""
        out = b""
        for b, _ts in self.seg:
            if len(out) >= n:
                break
            out += b[:n - len(out)]
        return out

    def try_take(self, n):
        """Take n bytes ONLY if they are already buffered; else None."""
        if self.avail() < n:
            return None
        return self._consume(n)

    def try_take_timed(self, n):
        """Take n buffered bytes and the ARRIVAL time of the segment they
        came from.  The StateMap pushes its updates in bursts, so a message
        parsed out of a burst must carry the time the BURST arrived, not the
        time we happened to parse it."""
        if self.avail() < n:
            return None, 0.0
        out, t = self._consume(n, with_time=True)
        return out, (t if t is not None else time.monotonic())

    def poll(self, timeout=0.005):
        """Do one non-blocking read into the buffer."""
        self.sock.settimeout(timeout)
        try:
            d = self.sock.recv(4096)
        except socket.timeout:
            return False
        except OSError:
            self.eof = True
            return False
        if not d:
            self.eof = True
            return False
        self.seg.append((d, time.monotonic()))
        return True

    def take(self, n, timeout=2.0):
        if not self._fill(n, timeout):
            return None
        out, t = self._consume(n, with_time=True)
        self.last_take = t if t is not None else time.monotonic()
        return out

    def _consume(self, n, with_time=False):
        out = b""
        t = None
        while len(out) < n:
            b, ts = self.seg[0]
            need = n - len(out)
            if len(b) <= need:
                out += b
                self.seg.pop(0)
                if t is None:
                    t = ts
            else:
                out += b[:need]
                self.seg[0] = (b[need:], ts)
        return (out, t) if with_time else out

    def u32(self, timeout=2.0):
        d = self.take(4, timeout)
        return None if d is None else struct.unpack(">I", d)[0]

    def u16(self, timeout=2.0):
        d = self.take(2, timeout)
        return None if d is None else struct.unpack(">H", d)[0]

    def u64(self, timeout=2.0):
        d = self.take(8, timeout)
        return None if d is None else struct.unpack(">Q", d)[0]

    def i64(self, timeout=2.0):
        d = self.take(8, timeout)
        return None if d is None else struct.unpack(">q", d)[0]

    def f64(self, timeout=2.0):
        d = self.take(8, timeout)
        return None if d is None else struct.unpack(">d", d)[0]

    def string(self, timeout=2.0):
        """uint32 byte-length + UTF-16BE."""
        n = self.u32(timeout)
        if n is None:
            return None
        if n == 0:
            return ""
        d = self.take(n, timeout)
        if d is None:
            return None
        try:
            return d.decode("utf-16-be", errors="replace")
        except Exception:
            return repr(d)


# ---------------------------------------------------------------- discovery

def parse_discovery(data):
    if len(data) < 4 or data[:4] != DISCOVERY_MAGIC:
        return None
    o = 4
    if len(data) < o + 16:
        return None
    token = data[o:o + 16]
    o += 16
    fields = []
    for _ in range(4):
        if o + 4 > len(data):
            return None
        n = struct.unpack_from(">I", data, o)[0]
        o += 4
        if o + n > len(data):
            return None
        fields.append(data[o:o + n].decode("utf-16-be", errors="replace"))
        o += n
    if o + 2 > len(data):
        return None
    port = struct.unpack_from(">H", data, o)[0]
    o += 2
    return {
        "token": token,
        "source": fields[0],
        "action": fields[1],
        "software": fields[2],
        "version": fields[3],
        "port": port,
        "raw": data,
    }


def discover(seconds=5.0, ip_filter=None, verbose=False):
    """Listen on UDP 51337 for the device's announcements."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    except OSError:
        pass
    s.bind(("", DISCOVERY_PORT))
    s.settimeout(0.5)

    found = {}
    end = time.monotonic() + seconds
    print("listening on UDP %d for %.1f s ..." % (DISCOVERY_PORT, seconds))
    while time.monotonic() < end:
        try:
            data, addr = s.recvfrom(4096)
        except socket.timeout:
            continue
        except OSError as e:
            print("  socket error: %s" % e)
            continue
        if verbose:
            print("--- raw from %s (%d bytes) ---" % (addr[0], len(data)))
            print(hexdump(data))
        d = parse_discovery(data)
        if d is None:
            print("%s  NOT a StageLinq packet (%d bytes):" % (now(), len(data)))
            print(hexdump(data, 96))
            continue
        d["ip"] = addr[0]
        key = (addr[0], d["token"])
        if key not in found:
            found[key] = d
            print("%s  DEVICE from %s" % (now(), addr[0]))
            print("      token     %s" % d["token"].hex())
            print("      source    %r" % d["source"])
            print("      action    %r" % d["action"])
            print("      software  %r" % d["software"])
            print("      version   %r" % d["version"])
            print("      tcp port  %d" % d["port"])
        else:
            found[key]["seen"] = found[key].get("seen", 1) + 1
    s.close()
    if not found:
        print("\nNO DEVICE FOUND.")
        print("  - is StagelinQ switched ON in the unit's Utility/Settings menu?")
        print("  - is the unit on this subnet (192.168.6.x)?")
        print("  - is UDP %d blocked?" % DISCOVERY_PORT)
    else:
        print("\n%d device(s) seen." % len(found))
    return found


# ------------------------------------------------------- main service conn

class Device:
    """A discovered device, and the TCP conversation with it."""

    def __init__(self, ip, port, token, name=""):
        self.ip = ip
        self.port = port
        self.token = token
        self.name = name
        self.services = {}      # name -> port
        self.offered = []       # [(name, port)] - what WE offer the device
        self.sock = None
        self.reader = None
        self.peer_request = None

    # -- the main connection ------------------------------------------
    def open_main(self, timeout=5.0):
        self.sock = socket.create_connection((self.ip, self.port), timeout=timeout)
        self.sock.settimeout(0.5)
        self.reader = Reader(self.sock)
        return self.sock

    def offer(self, names):
        """Tell the device what we want, from the main connection."""
        self.offered = [(n, self.sock.getsockname()[1]) for n in names]

    def request_services(self):
        """[u32 2][16-byte token]"""
        self.sock.sendall(struct.pack(">I", MSG_SERVICES_REQUEST) + self.token)

    def read_services(self, seconds=4.0):
        """Parse the service announcements until the list stops growing."""
        end = time.monotonic() + seconds
        last = -1
        while time.monotonic() < end:
            mt = self.reader.u32(timeout=0.5)
            if mt is None:
                if len(self.services) == last and time.monotonic() > end - seconds + 1.5:
                    break
                continue
            if mt == MSG_SERVICE_ANNOUNCEMENT:
                tok = self.reader.take(16)
                if tok is None:
                    break
                name = self.reader.string()
                port = self.reader.u16()
                if name is None or port is None:
                    break
                self.services[name] = port
                print("      service %-20s port %d" % (name, port))
                last = len(self.services)
            elif mt == MSG_TIMESTAMP:
                self.reader.take(16)
                self.reader.take(16)
                self.reader.take(8)
                print("      (timestamp message)")
            elif mt == MSG_SERVICES_REQUEST:
                tok = self.reader.take(16)
                self.peer_request = tok
                print("      (device sent us a services request, token %s)"
                      % (tok.hex() if tok else "?"))
                # ANSWER IT.  The device asks what WE offer before it will
                # list its own services, and the reference implementations
                # always reply.
                if self.offered:
                    reply = b"".join(
                        struct.pack(">I", MSG_SERVICE_ANNOUNCEMENT) + self.token
                        + enc_string(nm) + struct.pack(">H", pt)
                        for (nm, pt) in self.offered)
                    self.sock.sendall(reply)
                    print("      replied with our offer: %s"
                          % ", ".join(nm for nm, _ in self.offered))
            else:
                print("      unknown main message id %d - stopping" % mt)
                break
        return self.services

    def close(self):
        try:
            if self.sock:
                self.sock.close()
        except OSError:
            pass
        self.sock = None


_listener = None


def get_listener():
    """A socket we can be called back on.

    The unit only lists its services if the port we announced is one it can
    actually CONNECT to.  Announcing the main connection's port looks right
    but is not: that socket is connected, not listening, and the unit gives
    up about half the time.  Announcing a real listener works every time.
    """
    global _listener
    if _listener is None:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("0.0.0.0", 0))
        s.listen(4)
        _listener = s
    return _listener


def fresh_listener():
    """A NEW listening socket.

    The unit remembers the client it is serving, so a re-used listener can
    leave it holding a stale connection.  A fresh port each time is more
    reliable.
    """
    global _listener
    if _listener is not None:
        try:
            _listener.close()
        except OSError:
            pass
    _listener = None
    return get_listener()


def negotiate(dev, token_mode="device", self_announce=False, attempts=6):
    """Open the main connection and collect the service list.

    The unit only lists its services to a client it has SEEN announce
    itself, so we announce (broadcast AND unicast), then keep asking until
    it answers.  Asking again is harmless.

    Each attempt gets a FRESH token and a FRESH listener: the unit remembers
    the client it is dealing with, and a stale one gets ignored.
    """
    token = dev.token if token_mode == "device" else os_urandom16()
    dev.token = token
    print("  connecting to %s:%d ..." % (dev.ip, dev.port))
    dev.open_main()
    for attempt in range(1, attempts + 1):
        if token_mode != "device":
            dev.token = os_urandom16()
        token = dev.token
        lp = fresh_listener().getsockname()[1]
        if self_announce:
            print("  announcing OURSELVES (token %s, local port %d)" % (token.hex(), lp))
            announce_self(token, lp, device_ip=dev.ip)
            time.sleep(0.35)
        print("  services request #%d (token=%s, %s)" % (attempt, token.hex(), token_mode))
        dev.request_services()
        svcs = dev.read_services(2.0)
        if svcs:
            return svcs
        print("  no answer yet")
    print("  NO SERVICES ANSWERED with that token.")
    return {}


def os_urandom16():
    """A client token: 16 bytes with the top bit CLEAR.

    PyStageLinQ's notes on the Go say the token in a services request "must
    have its most-significant bit 0".  The Go's own tokens start with 0xfb
    and 0xe7 - both with the bit SET - so the bit tells the two apart.
    """
    b = bytearray(os.urandom(16))
    b[0] &= 0x7F
    return bytes(b)


def announce_self(token, port, name="stagelinq-probe", software="probe", version="1.0",
                  device_ip=None):
    """Broadcast our OWN discovery announcement.

    The protocol is symmetric: sources and sinks both announce.  The device
    appears to want to know who is asking before it lists its services, so
    this is not optional - and it is sent BOTH broadcast and unicast,
    because a broadcast that the unit misses means no services at all.
    """
    body = (DISCOVERY_MAGIC + token
            + enc_string(name) + enc_string("DISCOVERER_HOWDY_")
            + enc_string(software) + enc_string(version)
            + struct.pack(">H", port))
    targets = list(broadcast_addresses())
    if device_ip:
        targets.insert(0, device_ip)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        s.settimeout(0.5)
        for host in targets:
            try:
                s.sendto(body, (host, DISCOVERY_PORT))
            except OSError as e:
                print("  announce to %s failed: %s" % (host, e))
    finally:
        s.close()


def broadcast_addresses():
    """Every broadcast address of our interfaces."""
    out = []
    try:
        host = socket.gethostname()
        for info in socket.getaddrinfo(host, None, socket.AF_INET):
            ip = info[4][0]
            if ip.startswith("127."):
                continue
            parts = ip.split(".")
            if len(parts) == 4:
                parts[3] = "255"
                out.append(".".join(parts))
    except OSError:
        pass
    out.append("255.255.255.255")
    seen = set()
    uniq = []
    for a in out:
        if a not in seen:
            seen.add(a)
            uniq.append(a)
    return uniq


# ------------------------------------------------------------ BeatInfo svc

class BeatInfo:
    """The BeatInfo service: a live, continuous beat position per deck."""

    def __init__(self, dev, service_name="BeatInfo"):
        self.dev = dev
        self.name = service_name
        self.sock = None
        self.reader = None
        self.local_port = 0
        self.frames = []        # (wall, clock, rows) for every frame read

    def connect(self, announce_fmt="plain", token=None, announce=False):
        port = self.dev.services.get(self.name)
        if port is None:
            print("  no %s service" % self.name)
            return False
        self.sock = socket.create_connection((self.dev.ip, port), timeout=5)
        self.sock.settimeout(0.5)
        self.reader = Reader(self.sock)
        self.local_port = self.sock.getsockname()[1]
        tok = token or self.dev.token
        if announce:
            announce_self(tok, self.local_port, device_ip=self.dev.ip)
            time.sleep(0.35)
        ann = (struct.pack(">I", MSG_SERVICE_ANNOUNCEMENT) + tok
               + enc_string(self.name) + struct.pack(">H", self.local_port))
        if announce_fmt == "length":
            ann = struct.pack(">I", len(ann)) + ann
        self.sock.sendall(ann)
        print("  connected to %s on port %d, announced (fmt=%s, local port %d)"
              % (self.name, port, announce_fmt, self.local_port))
        return True

    def start_stream(self):
        """[u32 length=4][u32 0x00000000]"""
        self.sock.sendall(struct.pack(">II", 4, BEAT_START))
        print("  asked for the beat stream")

    def stop_stream(self):
        try:
            self.sock.sendall(struct.pack(">II", 4, BEAT_STOP))
        except OSError:
            pass

    def read_frames(self, seconds=10.0, verbose=False):
        """Read beat frames.  Handles both the length-prefixed form and a
        bare magic, because we are not 100% sure which the unit uses."""
        end = time.monotonic() + seconds
        count = 0
        self.wall0 = time.time()
        self.mono0 = time.monotonic()
        seen = []
        while time.monotonic() < end:
            x = self.reader.u32(timeout=0.05)
            if x is None:
                continue
            stamp = self.reader.last_take
            if x == BEAT_EMIT:
                payload = None
            elif x in (BEAT_START, BEAT_STOP):
                print("  [control message 0x%08x]" % x)
                continue
            else:
                # a length prefix
                body = self.reader.take(x, timeout=1.0)
                if body is None:
                    print("  truncated frame")
                    break
                if len(body) < 4:
                    continue
                magic = struct.unpack_from(">I", body, 0)[0]
                if magic != BEAT_EMIT:
                    if verbose:
                        print("  frame magic 0x%08x, %d bytes:" % (magic, len(body)))
                        print(hexdump(body, 96))
                    continue
                payload = body[4:]
            if payload is None:
                # bare magic: read the rest inline
                clock = self.reader.u64(timeout=1.0)
                decks = self.reader.u32(timeout=1.0)
                if clock is None or decks is None:
                    break
                rows = self._read_decks(decks)
            else:
                if len(payload) < 12:
                    continue
                clock = struct.unpack_from(">Q", payload, 0)[0]
                decks = struct.unpack_from(">I", payload, 8)[0]
                rows = self._decode_decks(payload[12:], decks)
            count += 1
            wall = self.wall0 + (stamp - self.mono0)
            self._print_frame(wall, clock, decks, rows)
            seen.append((wall, clock, rows))
            self.frames.append((wall, clock, rows))
        self.summary(seen)
        return count

    def summary(self, seen):
        """What the numbers say, once we have a few of them."""
        if len(seen) < 3:
            return
        print("\n  --- summary over %d frames ---" % len(seen))
        for deck in range(len(seen[0][2])):
            rows = [s[2][deck] for s in seen if len(s[2]) > deck]
            if len(rows) < 3:
                continue
            b0, b1 = rows[0][0], rows[-1][0]
            t0, t1 = seen[0][1], seen[-1][1]
            dt = (t1 - t0) / 1e9
            if dt <= 0:
                continue
            rate = (b1 - b0) / dt
            bpm = rows[-1][2]
            print("    deck%d: beat %.4f -> %.4f over %.3f s" % (deck + 1, b0, b1, dt))
            print("           measured rate %.4f beats/s   bpm/60 = %.4f   bpm = %.4f"
                  % (rate, bpm / 60.0 if bpm else 0.0, bpm))
            if bpm and abs(rate - bpm / 60.0) < 0.5:
                print("           -> the beat counter runs at the TEMPO: it is a beat count")
            # the phase we saw at arrival
            fr = [r[0] - math.floor(r[0]) for r in rows]
            print("           phase at arrival: min %.3f max %.3f" % (min(fr), max(fr)))
            # the optional sample positions
            if rows[0][3] is not None and rows[-1][3] is not None:
                ds = rows[-1][3] - rows[0][3]
                db = b1 - b0
                if db:
                    print("           samples %.0f -> %.0f  = %.2f samples per beat"
                          % (rows[0][3], rows[-1][3], ds / db))
                    print("           sample at beat 0 = %.0f"
                          % (rows[0][3] - rows[0][0] * (ds / db)))
            else:
                print("           (no sample positions in this frame)")

    def _read_decks(self, decks):
        out = []
        for _ in range(decks):
            b = self.reader.f64(timeout=1.0)
            t = self.reader.f64(timeout=1.0)
            p = self.reader.f64(timeout=1.0)
            if b is None:
                return out
            out.append([b, t, p, None])
        return out

    def _decode_decks(self, payload, decks):
        out = []
        off = 0
        for _ in range(decks):
            if off + 24 > len(payload):
                break
            b, t, p = struct.unpack_from(">ddd", payload, off)
            off += 24
            out.append([b, t, p, None])
        # the OPTIONAL per-deck sample positions, if the frame carries them
        if len(payload) - off >= 8 * len(out):
            for r in out:
                r[3] = struct.unpack_from(">d", payload, off)[0]
                off += 8
        return out

    def _print_frame(self, wall, clock, decks, rows):
        ts = datetime.datetime.fromtimestamp(wall).strftime("%H:%M:%S.%f")[:-3]
        print("%s  clock=%d decks=%d" % (ts, clock, decks))
        for i, r in enumerate(rows):
            b, t, p, s = r
            bar = None
            beat_in_bar = None
            if b is not None and not math.isnan(b):
                bar = int(math.floor(b / 4.0))
                beat_in_bar = (int(math.floor(b)) % 4) + 1
            extra = "" if s is None else "  samples=%.0f" % s
            print("      deck%d  beat=%s total=%s bpm=%s   -> bar %s, beat in bar %s%s"
                  % (i + 1, fmt(b), fmt(t), fmt(p), bar, beat_in_bar, extra))

    def close(self):
        try:
            if self.sock:
                self.sock.close()
        except OSError:
            pass


def fmt(v):
    if v is None:
        return "?"
    if isinstance(v, float) and math.isnan(v):
        return "nan"
    if isinstance(v, float):
        return "%.4f" % v
    return str(v)


# ------------------------------------------------------------ StateMap svc

class StateMap:
    """The StateMap service: named states, each a JSON value."""

    def __init__(self, dev, service_name="StateMap"):
        self.dev = dev
        self.name = service_name
        self.sock = None
        self.reader = None
        self.values = {}

    def connect(self, announce_fmt="plain", token=None, announce=False):
        port = self.dev.services.get(self.name)
        if port is None:
            print("  no %s service" % self.name)
            return False
        self.sock = socket.create_connection((self.dev.ip, port), timeout=5)
        self.sock.settimeout(0.5)
        self.reader = Reader(self.sock)
        local_port = self.sock.getsockname()[1]
        tok = token or self.dev.token
        if announce:
            announce_self(tok, local_port, device_ip=self.dev.ip)
            time.sleep(0.35)
        ann = (struct.pack(">I", MSG_SERVICE_ANNOUNCEMENT) + tok
               + enc_string(self.name) + struct.pack(">H", local_port))
        if announce_fmt == "length":
            ann = struct.pack(">I", len(ann)) + ann
        self.sock.sendall(ann)
        print("  connected to %s on port %d (local port %d)"
              % (self.name, port, local_port))
        return True

    def subscribe(self, path, interval=0):
        """[u32 len]['smaa'][u32 0x7d2][str path][u32 interval]"""
        body = SMAA_MAGIC + struct.pack(">I", SMAA_SUBSCRIBE) + enc_string(path) \
            + struct.pack(">I", interval)
        self.sock.sendall(struct.pack(">I", len(body)) + body)

    def read_states(self, seconds=6.0, verbose=False):
        end = time.monotonic() + seconds
        seen = 0
        while time.monotonic() < end:
            ln = self.reader.u32(timeout=0.5)
            if ln is None:
                continue
            body = self.reader.take(ln, timeout=1.0)
            if body is None or len(body) < 8:
                continue
            if body[:4] != SMAA_MAGIC:
                if verbose:
                    print("  not smaa: %s" % binascii.hexlify(body[:32]).decode())
                continue
            mtype = struct.unpack_from(">I", body, 4)[0]
            rest = body[8:]
            if mtype != SMAA_EMIT:
                continue
            # rest: str name, str json
            off = 0
            if len(rest) < 4:
                continue
            n = struct.unpack_from(">I", rest, off)[0]
            off += 4
            name = rest[off:off + n].decode("utf-16-be", errors="replace")
            off += n
            if len(rest) < off + 4:
                continue
            n = struct.unpack_from(">I", rest, off)[0]
            off += 4
            js = rest[off:off + n].decode("utf-16-be", errors="replace")
            seen += 1
            self.values[name] = js
            print("%s  %-42s = %s" % (now(), name, js))
        return seen

    def close(self):
        try:
            if self.sock:
                self.sock.close()
        except OSError:
            pass


# ------------------------------------------------------------------- modes

def pick_devices(found, ip):
    """Every announced identity on this IP.

    The PRIME GO announces itself under MORE THAN ONE token - we have seen
    'OfflineAnalyzer' twice with different tokens, plus 'JP11' (the Engine OS
    platform manager) - each with its own TCP service port.  They are not
    interchangeable, so we try them all.
    """
    cands = [d for d in found.values() if d["ip"] == ip]
    if not cands and len(found) == 1:
        cands = list(found.values())
    # the JP11 identity is the Engine OS one; try it first
    cands.sort(key=lambda d: (0 if d["software"].lower().startswith("jp") else 1,
                               d["software"], d["port"]))
    return cands


def mode_discover(args):
    discover(args.seconds, args.ip, args.verbose)
    return 0


def mode_capture(args):
    """Open the main connection and dump every byte that comes back."""
    found = discover(args.seconds, args.ip, args.verbose)
    cands = pick_devices(found, args.ip)
    if not cands:
        print("no device to talk to")
        return 1
    for d in cands:
        print("\n=========================================================")
        print("identity %r %s  token %s  port %d"
              % (d["software"], d["version"], d["token"].hex(), d["port"]))
        for mode in (["device", "random"] if args.token == "both" else [args.token]):
            print("\n--- token mode: %s ---" % mode)
            dev = Device(d["ip"], d["port"], d["token"], d["source"])
            negotiate(dev, mode, args.self)
            deadline = time.monotonic() + 3.0
            while time.monotonic() < deadline:
                chunk = dev.reader.take(4096, timeout=0.5)
                if chunk:
                    print("  extra bytes (%d):" % len(chunk))
                    print(hexdump(chunk, 256))
            dev.close()
    return 0


def mode_beat(args):
    found = discover(args.seconds, args.ip, args.verbose)
    cands = pick_devices(found, args.ip)
    if not cands:
        print("no device to talk to")
        return 1
    for d in cands:
        print("\n=========================================================")
        print("identity %r %s  token %s  port %d"
              % (d["software"], d["version"], d["token"].hex(), d["port"]))
        for mode in (["device", "random"] if args.token == "both" else [args.token]):
            print("\n--- token mode: %s ---" % mode)
            dev = Device(d["ip"], d["port"], d["token"], d["source"])
            negotiate(dev, mode, args.self)
            if "BeatInfo" not in dev.services:
                print("  this identity did not offer BeatInfo")
                dev.close()
                continue
            for fmtmode in (["plain", "length"] if args.announce == "both" else [args.announce]):
                print("\n--- announce format: %s ---" % fmtmode)
                bi = BeatInfo(dev)
                if not bi.connect(fmtmode):
                    continue
                bi.start_stream()
                n = bi.read_frames(args.frames_seconds, args.verbose)
                print("  %d beat frame(s) in %.1f s" % (n, args.frames_seconds))
                bi.stop_stream()
                bi.close()
                if n:
                    return 0
            dev.close()
    return 0


def mode_state(args):
    found = discover(args.seconds, args.ip, args.verbose)
    cands = pick_devices(found, args.ip)
    if not cands:
        print("no device to talk to")
        return 1
    for d in cands:
        print("\n=========================================================")
        print("identity %r %s  token %s  port %d"
              % (d["software"], d["version"], d["token"].hex(), d["port"]))
        for mode in (["device", "random"] if args.token == "both" else [args.token]):
            print("\n--- token mode: %s ---" % mode)
            dev = Device(d["ip"], d["port"], d["token"], d["source"])
            negotiate(dev, mode, args.self)
            if "StateMap" not in dev.services:
                print("  this identity did not offer StateMap")
                dev.close()
                continue
            for fmtmode in (["plain", "length"] if args.announce == "both" else [args.announce]):
                print("\n--- announce format: %s ---" % fmtmode)
                sm = StateMap(dev)
                if not sm.connect(fmtmode):
                    continue
                for p in (list(args.paths) + STATE_PATHS):
                    sm.subscribe(p)
                n = sm.read_states(args.frames_seconds, args.verbose)
                print("  %d state update(s)" % n)
                sm.close()
                if n:
                    return 0
            dev.close()
    return 0


def mode_all(args):
    rc = mode_beat(args)
    print("\n" + "=" * 70 + "\n")
    rc |= mode_state(args)
    return rc


# ---------------------------------------------------------------- the combo

CUE_PATHS = [
    "/Engine/Deck1/Track/SongName",
    "/Engine/Deck1/Track/TrackLength",
    "/Engine/Deck1/Track/SampleRate",
    "/Engine/Deck1/CurrentBPM",
    "/Engine/Deck1/Track/CuePosition",
    "/Engine/Deck1/CuePosition",
    "/Engine/Deck1/DeckIsMaster",
    "/Engine/Deck1/BeatGrid",
    "/Engine/Deck1/BeatGridOffset",
    "/Engine/Deck1/BeatInBar",
    "/Engine/Deck1/BeatNumber",
    "/Engine/Deck1/BeatPosition",
    "/Engine/Deck1/Phrase",
    "/Engine/Deck1/PhraseIndex",
    "/Engine/Deck1/Track/BeatGrid",
    "/Engine/Deck1/Track/BeatGridFirstBeat",
    "/Engine/Deck1/Track/BeatNumber",
    "/Engine/Deck1/Track/Phrase",
    "/Engine/Deck1/Track/CuePositionBeats",
    "/Engine/Deck1/Track/FirstBeat",
    "/Engine/Deck1/Track/GridOffset",
    "/Engine/Deck1/Track/AnalyzedBeatGrid",
    "/Engine/Deck1/AnalyzedBeatGrid",
    "/Engine/Deck1/Track/BeatGridMarkerCount",
    "/Engine/Deck1/Track/SampleRate",
]


def connect_with_retries(d, tries=10):
    """Keep trying until the unit lists its services.

    The unit only lists them to a client it has SEEN announce itself, and
    even then it misses about one time in five.  So: announce, ask, and if
    the answer does not come, go round again with a fresh token.
    """
    for i in range(tries):
        dev = Device(d["ip"], d["port"], os_urandom16())
        svcs = negotiate(dev, "random", True, attempts=1)
        if svcs:
            print("  connected on attempt %d" % (i + 1))
            return dev
        dev.close()
        time.sleep(0.5)
    return None


def mode_watch(args):
    """A live view of the deck: BPM, beat, bar, and the deck's own bar
    position - the two side by side, so the downbeat can be checked by eye.
    """
    found = discover(args.seconds, args.ip, args.verbose)
    cands = pick_devices(found, args.ip)
    if not cands:
        print("no device")
        return 1
    for c in cands:
        if c["software"].lower().startswith("jp"):
            d = c
            break
    else:
        d = cands[0]

    dev = connect_with_retries(d)
    if dev is None:
        print("could not get the service list")
        return 1

    bi = BeatInfo(dev)
    bi.connect()
    bi.start_stream()
    sm = StateMap(dev)
    sm.connect()
    for p in WATCH_PATHS:
        sm.subscribe(p)

    states = {}
    end = time.monotonic() + args.seconds_watch
    print("  %-8s %8s %9s %6s %6s %9s  %s"
          % ("time", "bpm", "beat", "bar", "beat/bar", "BeatNumber", "track"))
    lastline = None
    while time.monotonic() < end:
        sm.reader.poll()
        bi.reader.poll()
        # -- drain the state map
        while sm.reader.avail() >= 4:
            ln = struct.unpack_from(">I", sm.reader.peek(4), 0)[0]
            if not (8 <= ln <= 65536) or sm.reader.avail() < 4 + ln:
                break
            body, ts = sm.reader.try_take_timed(4 + ln)
            body = body[4:]
            if body[:4] != SMAA_MAGIC:
                break
            off = 8
            n = struct.unpack_from(">I", body, off)[0]
            off += 4
            name = body[off:off + n].decode("utf-16-be", "replace")
            off += n
            n = struct.unpack_from(">I", body, off)[0]
            off += 4
            js = body[off:off + n].decode("utf-16-be", "replace")
            try:
                states[name] = json.loads(js)
            except Exception:
                states[name] = js
        # -- drain the beat stream
        row = None
        while bi.reader.avail() >= 4:
            x = struct.unpack_from(">I", bi.reader.peek(4), 0)[0]
            if x == 2:
                if bi.reader.avail() < 16:
                    break
                bi.reader.try_take(4)
                bi.reader.try_take(8)
                decks = struct.unpack(">I", bi.reader.try_take(4))[0]
                if bi.reader.avail() < 24 * decks:
                    break
                rows = [struct.unpack(">ddd", bi.reader.try_take(24)) for _ in range(decks)]
                row = rows[0]
            elif x > 8 and bi.reader.avail() >= 4 + x:
                body, ts = bi.reader.try_take_timed(4 + x)
                body = body[4:]
                if len(body) >= 16 and struct.unpack_from(">I", body, 0)[0] == 2:
                    decks = struct.unpack_from(">I", body, 12)[0]
                    off = 16
                    rows = []
                    for _ in range(decks):
                        if off + 24 > len(body):
                            break
                        rows.append(struct.unpack_from(">ddd", body, off))
                        off += 24
                    if rows:
                        row = rows[0]
            else:
                break
        if row:
            beat, total, bpm = row
            bn = states.get("/Engine/Deck1/BeatNumber")
            bnv = bn.get("value") if isinstance(bn, dict) else None
            song = states.get("/Engine/Deck1/Track/SongName")
            song = song.get("string") if isinstance(song, dict) else ""
            line = "  %-8s %8.3f %9.4f %6d %6d %9s  %s" % (
                datetime.datetime.now().strftime("%H:%M:%S"),
                bpm, beat, int(beat) // 4, int(beat) % 4 + 1,
                ("%.4f" % bnv) if bnv is not None else "-", song)
            if line != lastline:
                print(line)
                lastline = line
        time.sleep(0.005)

    bi.close()
    sm.close()
    dev.close()
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["discover", "capture", "beat", "state", "watch", "all"])
    ap.add_argument("--ip", default=DEFAULT_IP, help="the device's IP (default %s)" % DEFAULT_IP)
    ap.add_argument("--seconds", type=float, default=5.0, help="how long to listen for discovery")
    ap.add_argument("--frames-seconds", type=float, default=10.0, help="how long to read the stream")
    ap.add_argument("--token", choices=["device", "random", "both"], default="both",
                    help="which token to send in the service request")
    ap.add_argument("--announce", choices=["plain", "length", "both"], default="both",
                    help="how to frame the service announcement")
    ap.add_argument("--verbose", action="store_true", help="dump raw packets")
    ap.add_argument("--self", action="store_true",
                    help="broadcast our OWN discovery announcement before asking")
    ap.add_argument("--seconds-watch", type=float, default=30.0,
                    help="how long the watch mode runs")
    ap.add_argument("--paths", nargs="*", default=[],
                    help="extra StateMap paths to subscribe to")
    args = ap.parse_args()

    print("StageLinq probe - device %s\n" % args.ip)
    return {
        "discover": mode_discover,
        "capture": mode_capture,
        "beat": mode_beat,
        "state": mode_state,
        "watch": mode_watch,
        "all": mode_all,
    }[args.mode](args)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\ninterrupted")
        sys.exit(130)

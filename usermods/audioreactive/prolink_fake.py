"""
A fake Pioneer DJ Link deck.

It speaks enough of the PDJLink protocol for the ProLink v3 usermod to
believe a master CDJ is on the network: it announces itself, it sends
status packets, and it sends a BEAT packet on every beat - and the BEAT
packet has to land ON the beat, because that is where the board anchors
the deck's clock.

The audio will not match what this sends - it is a clock and a number,
not a track - but the board's deck path can be exercised with it: the
BPM, the beat in the bar, the beat number and the beats elapsed all
come from here.

The packets, as the v3 usermod reads them:

  ANNOUNCE (port 50000, type 0x06, 54 bytes)
      0x0A  the type
      0x0C  the device name
      0x21  the player id (1..63)
      0x24  the device id (1..63) - the usermod refuses its own
      0x26  the MAC
      0x2C  the IP

  STATUS (port 50002, type 0x0A, 0x120 bytes)
      0x0A  the type
      0x21  the player id
      0x89  bit 5 set = this player is the master
      0x99  the effective pitch, 32 bits
      0xA0  the beats elapsed, 32 bits
      0xA6  the beat in the bar, 1..4

  BEAT (port 50001, type 0x28, 60 bytes)
      0x0A  the type
      0x21  the player id - must be the ACTIVE player
      0x54  the speed ratio, 32 bits fixed point (unity = 1048576)
      0x5A  the base BPM x 100
      0x5C  the beat number, 1..4

Usage:
    python usermods/audioreactive/prolink_fake.py --host 192.168.6.249 --bpm 128
    python usermods/audioreactive/prolink_fake.py --bpm 128 --delay 40 --start-beat 2
    python usermods/audioreactive/prolink_fake.py --bpm 128 --watch     # follow the tempo

--delay is how late the beat packet arrives, in milliseconds.  A real
deck's packet is about 40 ms late, and the board shifts its grid back
onto the audible beat by that amount, so a fake deck with --delay 40
exercises the same path.

It lives here, next to the AudioReactive usermod, because it is a way
to TEST the beat source without a Pioneer rig: the usermod's tempo
tracker arbitrates the deck's grid against its own estimate, and this
is a deck-shaped clock the tracker can be pointed at.
"""
import argparse
import json
import os
import socket
import struct
import sys
import tempfile
import time

MAGIC = bytes([0x51, 0x73, 0x70, 0x74, 0x31, 0x57, 0x6D, 0x4A, 0x4F, 0x4C])

PORT_ANNOUNCE = 50000
PORT_BEAT = 50001
PORT_STATUS = 50002

TYPE_ANNOUNCE = 0x06
TYPE_STATUS = 0x0A
TYPE_BEAT = 0x28

SPEED_UNITY = 1048576

# The beat in the bar the deck is on, and the beats elapsed.  They are
# the two numbers the board's thirty-two is built from.
BEATS_PER_BAR = 4
BEATS_PER_PHRASE = 32


def announce(player_id=1, device_id=1, name=b"FAKE DECK"):
    p = bytearray(54)
    p[0:10] = MAGIC
    p[0x0A] = TYPE_ANNOUNCE
    p[0x0C:0x0C + len(name)] = name
    p[0x20] = 0x01
    p[0x21] = player_id
    p[0x23] = 0x36
    p[0x24] = device_id
    p[0x25] = 0x01
    # the MAC (six bytes) and the IP of this machine
    import uuid
    p[0x26:0x2C] = uuid.getnode().to_bytes(6, "big")
    p[0x2C:0x30] = socket.inet_aton(socket.gethostbyname(socket.gethostname()))
    p[0x30] = 0x01
    p[0x34] = 0x01
    return bytes(p)


def status(player_id=1, beat_in_bar=1, beats_elapsed=0, pitch=0):
    p = bytearray(0x120)
    p[0:10] = MAGIC
    p[0x0A] = TYPE_STATUS
    p[0x21] = player_id
    p[0x89] = 0x20          # bit 5: this player is the master
    p[0x99:0x9D] = struct.pack(">i", pitch)
    p[0xA0:0xA4] = struct.pack(">I", beats_elapsed)
    p[0xA6] = beat_in_bar
    return bytes(p)


def beat(player_id=1, bpm=128.0, beat_number=1, speed=1.0):
    # Long enough for the beat number at 0x5C - the usermod reads that
    # far, so a 60-byte packet would be read out of bounds.
    p = bytearray(0x60)
    p[0:10] = MAGIC
    p[0x0A] = TYPE_BEAT
    p[0x21] = player_id
    p[0x54:0x58] = struct.pack(">I", int(round(speed * SPEED_UNITY)))
    p[0x5A:0x5C] = struct.pack(">H", int(round(bpm * 100)))
    p[0x5C] = beat_number
    return bytes(p)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="255.255.255.255",
                    help="the board's address, or the subnet broadcast")
    ap.add_argument("--bpm", type=float, default=128.0)
    ap.add_argument("--player", type=int, default=1, help="the player id (1..63)")
    ap.add_argument("--device", type=int, default=1, help="the device id (1..63)")
    ap.add_argument("--delay", type=float, default=40.0,
                    help="how late the beat packet arrives, in ms")
    ap.add_argument("--start-beat", type=int, default=1,
                    help="the beat number to start on, 1..4")
    ap.add_argument("--beat-offset", type=int, default=0,
                    help="rotate the deck's bar numbering against the board's, "
                         "in beats (0..3).  A real deck's downbeat is the "
                         "track's real downbeat, and the board's own numbering "
                         "is only a convention, so the two are normally "
                         "rotated against each other.  This is what puts that "
                         "rotation on the wire.")
    ap.add_argument("--start-elapsed", type=int, default=0,
                    help="the beats elapsed to start on")
    ap.add_argument("--phrase-beats", type=int, default=BEATS_PER_PHRASE,
                    help="how many beats a phrase is (the board's thirty-two)")
    ap.add_argument("--quiet", action="store_true", help="announce but send no beats")
    ap.add_argument("--watch", action="store_true",
                    help="follow the tempo the board's estimator is hearing, "
                         "so the two grids do not drift apart")
    args = ap.parse_args()

    # Only one fake deck at a time.
    #
    # Two decks beating at once do not make a louder wall - they FIGHT
    # over the board's clock, because every beat packet re-anchors it,
    # and the grid ends up jumping between the two.  That looks like the
    # lights going crazy, and it is not the music.
    #
    # The lock is a file in the temp directory holding the PID.  A lock
    # whose process is GONE is not a lock - that is a deck that was
    # killed rather than stopped - so it is cleared and the new deck
    # starts.  Otherwise a test that ends by killing the deck would
    # leave the next one unable to start.
    tmp = tempfile.gettempdir()
    lock = os.path.join(tmp, "prolink_fake_%d.lock" % os.getpid())
    for other in os.listdir(tmp):
        if not other.startswith("prolink_fake_") or not other.endswith(".lock"):
            continue
        path = os.path.join(tmp, other)
        try:
            with open(path) as f:
                pid = int(f.read().strip())
            alive = True
            if os.name == "nt":
                import subprocess
                r = subprocess.run(["tasklist", "/FI", "PID eq %d" % pid],
                                   capture_output=True, text=True)
                alive = str(pid) in (r.stdout or "")
            else:
                try:
                    os.kill(pid, 0)
                except OSError:
                    alive = False
            if alive:
                print("another fake deck is already running (pid %d) - "
                      "stop it first" % pid)
                return
        except Exception:
            pass
        try:
            os.remove(path)
        except OSError:
            pass
    with open(lock, "w") as f:
        f.write(str(os.getpid()))

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    try:
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
    except OSError:
        pass

    dest = args.host
    bpm = args.bpm
    beat_period = 60.0 / bpm
    print("fake deck: %d BPM, player %d, %d ms of delay, to %s"
          % (bpm, args.player, args.delay, dest))
    print("  announce every 1 s, status every 100 ms, a beat packet on every beat")
    if args.watch:
        print("  following the tempo the board is hearing")

    t0 = time.perf_counter()
    next_beat = t0 + args.delay / 1000.0
    next_announce = t0
    next_status = t0
    next_watch = t0
    beat_no = args.start_beat
    elapsed = args.start_elapsed

    while True:
        now = time.perf_counter()
        # Follow the music, if asked to: the board's own estimate of the
        # tempo, so the fake deck and the audio stay on one grid and the
        # comparison between them means something.
        #
        # The board also says where it is in the beat, so the fake deck can
        # be put ON the audible beat: the beat packet goes out the delay
        # AFTER the beat the board hears, which is what a real deck does.
        if args.watch and now >= next_watch:
            # Every two seconds: the board's web server wedges after
            # roughly sixty GETs, and a test that reads the board too
            # often is what takes it down.  The phase is re-anchored from
            # the board's own beat, so two seconds is plenty - the drift
            # between the anchors is a fraction of a millisecond.
            next_watch = now + 2.0
            try:
                import urllib.request
                with urllib.request.urlopen(
                        "http://%s/json/ar" % dest, timeout=3) as r:
                    t = json.loads(r.read().decode("utf-8"))
                tt = t["AudioReactive"]["tempo"]
                nb = tt.get("bpm")
                if nb and nb > 20:
                    bpm = float(nb)
                    beat_period = 60.0 / bpm
                prog = tt.get("beat_progress")
                if prog is not None and tt.get("beat_in_bar"):
                    # The time to the next beat, plus the delay: the
                    # packet has to LAND the delay late, the way a deck's
                    # does, or the board has nothing to shift back.
                    remaining = (1.0 - float(prog)) * beat_period
                    next_beat = now + remaining + args.delay / 1000.0
                    # The deck's own numbering, ROTATED against the board's.
                    # The board's beat-in-bar is a convention (an audio-only
                    # tracker cannot know the downbeat); the deck's is the
                    # track's real one.  So the two are normally a beat or
                    # two apart, and that is what --beat-offset puts on the
                    # wire.
                    bb = int(tt.get("beat_in_bar"))
                    beat_no = ((bb - 1 + args.beat_offset) % BEATS_PER_BAR) + 1
            except Exception:
                pass
        # The announce: the board needs a peer before it will believe a
        # status packet from it.
        if now - next_announce >= 1.0:
            next_announce = now
            s.sendto(announce(args.player, args.device), (dest, PORT_ANNOUNCE))
        # The status: this is what makes the player the ACTIVE one and
        # the master.
        if now - next_status >= 0.1:
            next_status = now
            s.sendto(status(args.player, beat_no, elapsed),
                     (dest, PORT_STATUS))
        # The beat: it has to LAND on the beat.
        if not args.quiet and now >= next_beat:
            while next_beat < now:
                next_beat += beat_period
            s.sendto(beat(args.player, bpm, beat_no), (dest, PORT_BEAT))
            beat_no = beat_no % BEATS_PER_BAR + 1
            elapsed += 1
        time.sleep(0.002)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nstopped")
        sys.exit(0)
    finally:
        # The lock goes when the deck does, however it goes - a deck that
        # is killed still has to be able to be replaced.
        for f in os.listdir(tempfile.gettempdir()):
            if f.startswith("prolink_fake_") and f.endswith(".lock"):
                p = os.path.join(tempfile.gettempdir(), f)
                try:
                    with open(p) as fh:
                        if int(fh.read().strip()) == os.getpid():
                            os.remove(p)
                except Exception:
                    pass

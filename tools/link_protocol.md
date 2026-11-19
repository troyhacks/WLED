# Ableton Link on this network - what is decoded, and what is not

Measured on 2026-11-19 against Ableton Live 12 Lite, 192.168.6.186, with
the tempo known to be 120 BPM.

## The transport

    UDP multicast 224.76.78.75
      :20808   the ASDP announce channel, ~18 packets/s, 107 bytes
      :20909   the ablsd start/stop channel, 1/s, 17 bytes

The widely-cited `224.0.0.87:20800` is the Link SDK's own default and is
**not** what Live 12 uses. The real group is in
`netsh interface ipv4 show joins`.

## An ASDP packet

    "_asdp_v"          7 bytes of magic
    01 01 05 00 00     version, message type, flags
    <12 bytes>         the session id
    <records>          each: uint32 length, the value, then the name

The records are `sess` (24 bytes), `stst` (8 bytes) and `mep` (6 bytes).

### The "sess" record

    +0   uint32   0
    +4   uint32   MICROSECONDS PER BEAT   ->  the tempo is 60000000 / this
    +8   uint32   0
    +12  uint32   a counter that marches at 2e6 per second
    +16  uint32   0
    +20  uint32   a clock that marches at 1e6 per second (microseconds)

The tempo field read 500000 - **120.0000 BPM** - steady across every
packet.

## The phase, and the one thing that is missing

The clock at +20 gives a phase that marches at exactly one turn per beat:

    phase = ((clock + K) mod microseconds_per_beat) / microseconds_per_beat

for any anchor K. The RATE is therefore solved. The **anchor is not**:
K is what puts the wrap on the music's downbeat.

**The anchor is not in the passive traffic, and this was proved rather than
assumed.** Live's transport was stopped and restarted during a capture -
it went back to 1.1.1 - and the clock and the phase marched straight
through it: 207.6 s of clock over 211.7 s of wall, with no reset. A
phase tied to the transport would have jumped; this one is a free-running
session phase, so its relation to the music's downbeat changes every time
the transport restarts.

**=> A follower needs Live's state, and Link only shares that with real
peers.** Announcing ourselves with a valid `sess` record gets us ignored
(`tools/link_peer.py`); the announce's `mep` record carries Live's peer
address and a gateway port that is not listening. The peer handshake is
the remaining piece, and Ableton's LinkKit is the reference.

## What this means for the board

A Link follower would take the **tempo** and the **phase**, and would
still need the board's own downbeat mark - the same escape hatch as every
other source - because the phase's relation to the music's downbeat is not
something Link publishes to a passive listener.

The board-side pieces are already understood: join the multicast group on
:20808, parse the ASDP records, take the tempo from `sess`+4, and derive
the phase from `sess`+20. The peer handshake is the part that is not.

## The tools

    tools/link_probe.py    find Link on the network and dump the traffic
    tools/link_decode.py   parse it and decode the tempo
    tools/link_peer.py     the peer announce experiment (the next step)
    tools/link_origin.py   watch the transport being restarted, for the anchor

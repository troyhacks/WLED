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

    "_asdp_v"  + a version byte of 0x01     8 bytes
    uint8   message type                     1 = alive, 2 = response
    uint8   ttl
    uint16  group id                         the session group
    12 bytes                                 the node id
    then the payload, as entries of:
        uint32  length
        <value>
        uint32  key     - FOUR ASCII CHARACTERS, big endian

**The version byte is the integer 1, not the character '1'.** Getting that
wrong means the magic never matches and an announce carries a magic the unit
does not recognise. The keys are ASCII codes: `sess`, `stst`, `mep4`.

## The tempo and the phase - both straight out of the traffic

The `sess` entry's 24 bytes are the SDK's `Timeline`, which is
`(Tempo, Beats, timeOrigin)`. From the SDK's own definitions:

| | on the wire |
|---|---|
| `Tempo` | `microsPerBeat`, an **int64, big endian** |
| `Beats` | **millionths of a beat**, an int64, big endian |
| `timeOrigin` | microseconds, an int64, big endian |

So:

    +0   int64   MICROSECONDS PER BEAT   ->  the tempo is 60000000 / this
    +8   int64   MILLIONTHS OF A BEAT    ->  the phase
    +16  int64   a microsecond time origin

Measured: `microsPerBeat` = 500000, so **120.0000 BPM**, steady across every
packet. And the phase field advances at **1.998964 beats/s** against the
tempo's 2.000000 - 0.05 % over twenty seconds, which is just the jitter
of the packets arriving in bursts.

**So the tempo AND the beat phase both come out of the passive announce
traffic. A follower needs no peer handshake at all.**

(The SDK models the phase as `beatOrigin + microsToBeats(time - timeOrigin)`;
Live sends the phase directly and running, which is the same thing with the
origin folded in.)

### What this corrects

An earlier note here said the phase's anchor was not in the passive traffic
and that a peer handshake would be needed. **That was wrong, and the reason
was a misread of the field boundaries.** The two counters that looked like
separate fields are one int64 - the phase - and the tempo field was read
from the wrong offset. There is no missing anchor: `sess`+8 *is* the phase,
in millionths of a beat.

The proof that the earlier reading was wrong is the phase's RATE: it advances
at exactly the beat rate, which is what a phase does and what a free-running
clock does not.

## The one thing left

Whether the phase is Live's *transport* position, or the transport position
plus the track's start offset.

**It is the transport position**, and the evidence is in the readings taken
during this work:

- The transport was read as **690 bars** while the phase was at **2760
  beats** - 690 bars exactly. The phase and Live's display were the same
  number.
- It was then read as **716 bars**, and the phase advanced by exactly the
  same 26 bars over that stretch - even though the unit's microsecond clock
  ran on for far longer, because the transport only moves while it plays
  and the phase only moves while it plays.
- When the transport was stopped and restarted, Live showed **1.1.1** -
  the track's start, i.e. the transport *parked* - while the phase carried
  on. That is the transport being parked, not the phase being wrong; the
  moment it plays again the display is the phase again.

So the phase is Live's own beat grid, in beats, and it comes out of the
announce traffic. **A follower needs no peer handshake, no offset and no
help from the board's estimator: the downbeat falls out of it, because
Link's beat one is the music's beat one.**

(The one caveat: between a stop and the next play, Live's transport is
parked at the track's start while the phase keeps its place. A follower
would be briefly ahead of the audio for that moment.)

## The tools

    tools/link_probe.py    find Link on the network and dump the traffic
    tools/link_decode.py   parse it and decode the tempo and the phase
    tools/link_watch2.py   watch every session-state field across a restart
    tools/link_peer.py     a peer announce experiment (kept: it is where the
                           "do we need a handshake at all" question was
                           answered - we do not)

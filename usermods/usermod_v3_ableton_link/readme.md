# Ableton Link usermod - the third sync source

Ableton Live's **Link** is Ableton's clock sync: apps on the LAN share a
tempo and a beat phase. It is the same idea as a DJ deck publishing its
own clock, arriving from the other direction - a DAW rather than a deck.

This usermod listens for it and publishes the phase to the core's beat
clock, so every beat-locked effect on the board follows Live.

## The order of preference

The core arbitrates between the sources in the order they were written:

1. **Pioneer**, over Pro DJ Link
2. **Denon DJ**, over StageLinq
3. **Ableton Live**, over Link

Only one of them drives at a time - the first one that is there wins. The
decks come first because they publish the music's own grid, and Live is the
one that is there when nothing else is.

## What Link publishes, and how it is read

A unit broadcasts itself on **UDP 224.76.78.75, port 20808**, about
eighteen times a second. Each packet carries a session state - the SDK's
`Timeline` - as a 24-byte record called `sess`:

| offset | type | meaning |
|---|---|---|
| +0 | int64 BE | `microsPerBeat` - the tempo is `60000000 / this` |
| +8 | int64 BE | the phase, in **millionths of a beat** |
| +16 | int64 BE | a microsecond time origin |

The phase advances at exactly the beat rate, so the bar and the beat in the
bar fall straight out of it - and the downbeat comes with them, because
Link's beat one is the music's beat one.

**No peer handshake, no offset, and no help from the board's own
estimator.** The whole wire format, and the measurements behind it, are in
[tools/stagelinq_protocol.md](../../tools/stagelinq_protocol.md).

## Settings

Under `Ableton_Link`:

| key | type | default | meaning |
|---|---|---|---|
| `enabled` | bool | false | the usermod runs |
| `enable_debug` | bool | true | verbose logging |
| `Multicast_Group` | string | `224.76.78.75` | the group the unit announces on |
| `Multicast_Port` | number | `20808` | the port the unit announces on |
| `Phase_Offset_ms` | number | 0 | shifts the grid, for a phase that is a shade out |
| `Stale_ms` | number | `2000` | no packets for this long = the session is gone |

The group and the port are settings rather than constants because the
Link SDK's own defaults differ from what Live 12 uses; if a different
version of Live announces on a different group, this is where to say so.

## Readouts

* The web GUI's tempo panel shows the source as **ABLETON LINK**.
* `/json/ar` has an `AbletonLink` object: the tempo, the beat, the phase in
  beats, and how many packets have arrived.
* The info pane has two rows: the session and its rate, and the bar and the
  beat in the bar.

## The one caveat

Between a stop and the next play, Live's transport is parked at the track's
start while the phase holds its place - so a follower is briefly ahead of
the audio for that moment. Live re-anchors when it plays again.

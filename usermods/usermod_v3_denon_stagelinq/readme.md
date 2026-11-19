# Denon DJ StageLinq usermod

A Denon DJ unit - a PRIME GO, a PRIME 4, a PRIME 2 - speaks **StageLinq**,
Denon's network protocol for lighting and video control. This usermod speaks
it and follows the deck's clock, exactly as the Pro DJ Link usermod follows a
Pioneer CDJ.

## What it gives the board

| | |
|---|---|
| **BPM** | the deck's own tempo |
| **the beat** | a continuous, absolute beat position - the playhead's place in the track, to the fraction of a beat |
| **the bar** | the beat in the bar (1..4) and the bar number |
| **the master deck** | which deck the unit has made the master |

Every beat-locked effect works with a Denon deck exactly as it does with a
Pioneer one: the Beat Strobe, the Beat Sweep, the phrase counter, the
`#BPM` and `#BEAT` text macros.

## What it does not give the board

**The song's phrase.** StageLinq has no phrase structure at all - there is
no `Phrase` state path, and Engine's own beatgrid format holds only beat
markers, with no section labels. The Pioneer's PSSI phrase table, which
gives Intro/Verse/Chorus, has no Denon equivalent.

So in the web GUI the phrase checkbox is hidden when a StageLinq deck is
driving, and the board's internal 1/2/4/8-bar counter is all there is.

The **downbeat** is known, though. The deck's beat counter starts at the
deck's cue, which is the analysed downbeat - so beat zero is the downbeat
and the bar is simply the beat over four. The user's own downbeat mark (the
`tempoDown` button, or `j` on the console) overrides it, exactly as it does
with a Pioneer deck.

## How it finds the deck

**It does not have to be told.**

A Denon unit broadcasts itself on UDP 51337 once a second. The board
listens, and the announcement carries both the unit's address and the port
it wants to be reached on. Nothing has to be configured.

Three things have to be right, and all three were measured against a PRIME
GO (see `tools/stagelinq_protocol.md`):

1. **The token in the services request must be ours**, with the top bit of
   the first byte clear. The unit's own tokens all have that bit set, and a
   request carrying one of those is ignored.
2. **The board must announce itself first.** Without an announcement the
   unit never answers - 0 out of 8 tries.
3. **The port in the announcement must be one the board is listening on.**
   The unit tries to connect back to it.

The unit itself is racy - it answers about four times in five - so the
usermod keeps asking, with a fresh token and a fresh listener each time.

## Settings

Under `Denon_StageLinq`:

| key | type | default | meaning |
|---|---|---|---|
| `enabled` | bool | false | the usermod runs |
| `enable_debug` | bool | true | verbose logging |
| `Player_IP` | string | `""` | the unit's address. Leave it empty: the unit finds itself |
| `Deck` | number | 0 | 0 = the master deck; 1..4 = pin one |
| `Phase_Offset_ms` | number | 0 | shifts the whole grid, for a grid that is a little out |
| `Anchor_Max_Beats` | number | 4 | the grid dies after this many beats without a beat; 0 = never trust the deck |
| `Connect` | bool | true | when off, the board asks nothing of anything |

`Connect` is the safe mode. StageLinq is a client protocol - the board has
to open TCP connections to the unit - so unlike the ProLink's listen-only
mode there is no way to be on the network without saying something. This
switch is the nearest thing: with it off, no socket is opened at all.

## Readouts

* The web GUI's tempo panel shows the source as **STAGE LINQ**, and the
  phrase checkbox is hidden.
* `/json/ar` shows `tempo.source` as `STAGE LINQ`, and `tempo.source_id`
  as `2`.
* `/json/ar` also has a `StageLinq` object with the deck's own numbers.
* The info pane has three rows: the unit and its rate, the bar and the beat
  in the bar, and the track.

## The protocol

`tools/stagelinq_protocol.md` is the measured wire format, and
`tools/stagelinq_probe.py` is the host-side client that proved it - it can
watch a deck from the PC:

```
python tools/stagelinq_probe.py watch --ip 192.168.6.131
```

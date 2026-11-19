# StageLinq on the Denon DJ PRIME GO — the validated wire protocol

Everything in this file was measured against a **PRIME GO** on
**192.168.6.131**, Engine OS **5.3.0**, over wired Ethernet, with the
tools in `tools/stagelinq_probe.py` and `tools/stagelinq_align.py`.

StageLinq is Denon DJ's network protocol for lighting and video control.
The device broadcasts itself on UDP 51337 and then offers a set of TCP
services on dynamic ports.

---

## 1. Discovery (UDP)

The device broadcasts to `255.255.255.255:51337` from a random source
port. A client binds UDP 51337 and reads. **All integers are big-endian.**
A *network string* is `uint32` length **in bytes**, then UTF-16BE data.

> **THE STRING ENCODING IS BIG ENDIAN, AND IT WILL BITE YOU.**
>
> UTF-16BE puts the **high** byte of each code unit first. For an ASCII
> character that high byte is zero, so the bytes of `'p'` are `00 70` and
> **the character is the SECOND byte of each pair**. Reading the first byte
> gives you a string of NULs — which looks like an empty string, matches
> nothing, and fails silently.
>
> This is the easiest thing to get wrong in this protocol, and it fails in
> the most confusing way: the discovery packet still parses, the port still
> comes out right, and the unit is found — but its name is empty and no
> service name ever matches, so nothing ever connects.

```
[4]  magic "airD"
[16] token (opaque)
[str] source          the device name
[str] action          "DISCOVERER_HOWDY_" / "DISCOVERER_EXIT_"
[str] software name
[str] software version
[2]  TCP port         the device's service-request port
```

**The PRIME GO announces itself THREE times, under three different tokens:**

| software | version | token (first bytes) | TCP port |
|---|---|---|---|
| `JP11` | 5.3.0 | `fb670035…` | 46663 |
| `OfflineAnalyzer` | 1.0.0 | `257be0eb…` | 35509 |
| `OfflineAnalyzer` | 1.0.0 | `e7f8b9c0…` | 42727 |

**Only the `JP11` identity serves the interesting services.** The two
`OfflineAnalyzer` identities do not answer a services request at all.
`JP11` is the Engine OS platform manager, so it is the one to talk to.

**How often, and how stable:**

| identity | announcements | period | TCP port |
|---|---|---|---|
| `JP11` 5.3.0 | 12 in 12 s | **exactly 1.000 s** (0.985–1.015) | **46663**, unchanged for the whole session |
| `OfflineAnalyzer` 1.0.0 | 24 in 12 s (two identities) | ~1 s each, interleaved | 35509 and 42727 |

So the unit tells us where it is once a second, and its port does not
move while it is up.

The device also **connects back** to us (from an ephemeral port) with its
own `ServicesRequest`; that callback carries the `OfflineAnalyzer` token
and is not useful. It only happens *after* we have opened the TCP
connection, so it is not a discovery route.

## 1a. Do we need to know the IP?

**No — it is a broadcast, and the board can be told nothing.**

The device broadcasts itself on UDP 51337. The client's UDP receive
socket gives it two things for free: the device's IP is the **source
address** of the packet, and the device's TCP port is **in the packet**.
No configuration is needed, and within one second of binding the socket
the board has both.

**But there is a catch, and it is the one that matters.** We also have to
announce *ourselves*, and the device ignores us unless that announcement
reaches it. Measured, connecting to a known IP:port and never listening
for broadcasts:

| | service list answered |
|---|---|
| no UDP listen, **no** announcement | **0 / 8** |
| no UDP listen, **unicast** announcement to the device's IP | **7 / 8** |

So the announcement is the load-bearing part, not the discovery. It
should be sent **both broadcast and unicast** — the unicast needs the IP,
which we have from the discovery packet's source address.

If the board and the unit are on different subnets the broadcast never
arrives, so a static `Player_IP` is a useful fallback — but then the
**port** is also needed, and only the broadcast carries it. The port has
been stable (46663) for the whole session, but it cannot be assumed to
survive a reboot. The right design is therefore: **listen for broadcasts
all the time, cache what they say, and let the `Player_IP` setting
override the address only.**

## 2. The handshake — this is the part that is easy to get wrong

```
1. open a TCP LISTENING socket and note its port
2. TCP connect to the port the device advertised
3. NOW announce ourselves, with the connection open:
       "airD" + <16-byte token> + source + "DISCOVERER_HOWDY_" + software + version + <port>
   (the port must be one we are actually LISTENING on — see below)
   send it BOTH broadcast and unicast to the device
4. wait a moment, then send:  [u32 2][16-byte token]        a services REQUEST
5. read:  [u32 2][16-byte device token]  the device's own request
          [u32 0][16-byte device token][str service name][u16 port]   ... one per service
          [u32 1][16-byte device token][16 zero bytes][u64 ns]        ... sometimes a timestamp
```

**THE ORDER MATTERS: open the connection FIRST, then announce, then ask.**
The unit ties the announcement to the connection that is already open. An
announcement that arrives *before* the connection is an announcement about
a client it has no connection for, and it is never answered — which looks
exactly like a unit that is merely busy, and is very hard to tell apart.

### The three rules that make it work

1. **The token must be OURS, with the top bit CLEAR.** The device's own
   tokens start `fb`, `e7` — top bit set. Sending the device's token back
   is silently ignored. (PyStageLinQ's notes say the same: "its
   most-significant bit must be 0".)
2. **We must announce ourselves FIRST.** Without the announcement the
   device answers **0 times in 8**. With it, 6–7 times in 8.
3. **The announced port must be one we are LISTENING on.** Announcing the
   main connection's own port looks right but is not — that socket is
   *connected*, not *listening*, and the device gives up about half the
   time. With a real listener: 6/6, then 7/10.

The residual ~20% flake is the device's own race. **Retry**: close, wait,
announce again with a fresh token and a fresh listener, ask again. Two or
three attempts make it certain.

**And a trap of our own making:** we are listening on the very port we
broadcast to, so **our own announcement comes back to us**. If we take it
for the unit's, the unit's address is overwritten with ours and the
service connections are dialled straight back to this board — which then
talks to itself and never connects to anything. The token cannot be used
to tell the two apart, because the token changes every time we
reconnect; **the name in the announcement is what tells them apart.**

**And KEEP ANNOUNCING.** The unit expires a client that stops announcing
itself. Measured, on the same connection and with the same token:

| what the client does | what happens |
|---|---|
| announces once, then goes quiet | dropped after **3 seconds** |
| re-announces every 2 s | **never dropped** (25 s, 60 kB) |
| re-announces every 1 s | **never dropped** |

So the announcement is not a one-off registration but a **heartbeat** —
and it must carry the **same token** every time, so that it is a refresh
and not a new registration. A refresh with a fresh token would register a
new client and drop the old one.

### What we must reply

The device sends *us* a `ServicesRequest` (type 2) as well. The reference
implementations answer it with their own `ServicesAnnouncement`s. We do
too (`dev.offered`), but it does not change the success rate — the
announcement is what matters.

## 3. The services

```
StateMap             port 34419
Broadcast            port 36537
TimeSynchronization  port 37741
BeatInfo             port 38253
FileTransfer         port 40139
```

The ports are stable across connections but must be read, not assumed.

**Both StateMap and BeatInfo can be open at the same time, on the same
token and the same main connection.** (Opening a second service with a
*different* token fails; the unit binds a client to the service it was
asked for first. So: one token, one main connection, both services open.)

### Opening a service

```
TCP connect to the service's port
send: [u32 0][16-byte token][str service name][u16 our local port]
```

### BeatInfo

```
send: [u32 4][u32 0x00000000]     start the stream
send: [u32 4][u32 0x00000001]     stop the stream
read: [u32 length][u32 0x00000002][u64 clock][u32 deckCount]
        per deck:  f64 beat, f64 totalBeats, f64 bpm
        per deck:  f64 samples   (optional - present when 8 more bytes per deck remain)
```

`length` counts every byte after itself, magic included.

| field | meaning |
|---|---|
| `clock` | the device's own clock, in nanoseconds |
| `beat` | **the absolute beat position of the playhead** — continuous and fractional |
| `totalBeats` | the track's total beats |
| `bpm` | the track's tempo |
| `samples` | the playhead position in samples |

Measured: the beat counter advances at **exactly** `bpm/60` beats per
second (2.149868 measured vs 2.149864 expected), and `samples` fits
`samples = cue + samplesPerBeat × beat` with **zero** residual.

### StateMap

```
subscribe: [u32 length]["smaa"][u32 0x000007d2][str path][u32 interval]
emit:      [u32 length]["smaa"][u32 0x00000000][str name][str json]
```

The JSON carries a `type` and the payload under a matching key:
`{"type":0,"value":129.99}` (number), `{"type":1,"state":true}` (bool),
`{"type":8,"string":"..."}` (string), `{"type":10,"value":17538095}`
(samples), `{"type":14,"value":2962.47}` (double).

### The StateMap paths that matter

| path | what |
|---|---|
| `/Engine/Deck{N}/BeatNumber` | **the deck's own beat position inside the bar, 0..4, continuous, wrapping** — the downbeat is where it crosses 0 |
| `/Engine/Deck{N}/Play` | playing |
| `/Engine/Deck{N}/PlayState` | transport state |
| `/Engine/Deck{N}/CurrentBPM` | the live tempo |
| `/Engine/Deck{N}/DeckIsMaster` | which deck is the master |
| `/Engine/Deck{N}/Track/SongName` | track title |
| `/Engine/Deck{N}/Track/ArtistName` | artist |
| `/Engine/Deck{N}/Track/CuePosition` | the main cue, in samples |
| `/Engine/Deck{N}/Track/SampleRate` | 44100 |
| `/Engine/Deck{N}/Track/TrackLength` | in samples |

`BeatNumber` is published roughly every 46 ms and its value is quantised
to about 0.075 beat, and it **lags** by up to one publication period.
That makes it useless as a live phase reference — but it is the
*definition* of where the bar starts.

## 4. Where the downbeat is

This is the one thing StageLinq does not state outright, and it was
measured.

- The BeatInfo beat counter's **zero is the deck's main cue**. The fit
  `samples = 28378.0 + 20538.9812 × beat` has a **zero residual**, and
  28378 is *exactly* the `Track/CuePosition` for that track.
- `BeatNumber` wraps past 4 and back to 0 at the downbeat. Correlating
  the two streams puts the downbeat at an absolute beat of
  `0 + 4k`.

So, for the PRIME GO:

```
beatInBar   = floor(beat) mod 4 + 1
bar         = floor(beat / 4)
beatProgress= frac(beat)
beatsElapsed= floor(beat)
```

The `0 + 4k` result is a *measurement*, not an assumption: the
`(beat − BeatNumber) mod 4` samples ranged over 0.016 … 0.125, i.e. from
zero up to one StateMap publication period (0.099 beat) of lag — which is
only possible if the true offset is zero.

**`BeatNumber` must not be used for the live bar position**: it lags by up
to 46 ms, so its integer part can be a beat out near a bar boundary. Use
the beat counter, and use `BeatNumber` only to establish the offset.

## 5. What StageLinq does NOT have

**There are no phrase / section markers.** No `Phrase` state path exists,
and Engine's own beat-grid format (reverse-engineered by the Mixxx wiki
from the Engine Library's `p.db` → `PerformanceData` → `beatData`) holds
only beat markers — sample offset, beat index, beats-to-next — and no
section labels. EAAS's `beat_grid` blob is that same label-free
structure.

So the Intro/Verse/Chorus table that the Pioneer PSSI gives us has **no
Denon equivalent**. The internal 1/2/4/8-bar counter has to be derived
from the deck's beat grid, which is what `bar` above already is.

## 6. Other observations

- `Speed` is the **pitch fader position** (0..1, 0.5 = centre), not a
  rate multiplier: a deck playing at `Speed = 0.5` still advances its
  beat at exactly `bpm/60`.
- The device announces itself roughly twice a second.
- The service ports are stable for the life of the unit, but they must
  still be read from the announcement.
- Everything works over plain Ethernet. The Prime Go also has Wi-Fi, but
  StageLinq over Wi-Fi is untested here; the TimecodeLink guide for Denon
  says "Use wired Ethernet. We haven't tested the players over Wi-Fi."

## 7. The tools

```
python tools/stagelinq_probe.py discover              # who is out there
python tools/stagelinq_probe.py watch --ip 192.168.6.131   # live beat/bar/track
python tools/stagelinq_probe.py beat  --ip 192.168.6.131   # the beat stream
python tools/stagelinq_probe.py state --ip 192.168.6.131   # the StateMap
python tools/stagelinq_align.py --ip 192.168.6.131    # where is the downbeat
```

Standard library only; nothing to install.

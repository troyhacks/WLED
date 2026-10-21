# usermod_v3_deck — Soomfon CN003 as a WLED control surface
> **READ THIS FIRST — session handoff, 2026-10-20.**
> The usermod is **stuck at Phase 0**: the deck enumerates and outbound commands
> work, but **no input has ever arrived on the interrupt IN pipe**. Everything
> below marked UNVERIFIED is still unverified. The last action taken was
> **removing the IN watchdog** (see "Watchdog removed") — that has NOT been tested
> on hardware yet. Read **"Session handoff — open questions and next steps"** at
> the end of this file before touching any code: the previous session's headline
> conclusion turned out to rest on a contaminated experiment.


ESP32-P4 running WLED acts as the USB **host** for a Soomfon CN003 / Mirabox
N4-family stream deck. No PC, no OpenDeck, no emulation: the P4 enumerates the
deck, reads its keys and rotary encoders, drives WLED state, and paints the
deck's LCD keys.

## Hardware layout

```
+------------------------------------------+
|  [1] [2] [3] [4] [5]                      |  5 wide
|  [6] [7] [8] [9] [10]                     |  2 tall  = 10 LCD keys
+------------------------------------------+
|  touch strip (full width, ~same height)   |
+------------------------------------------+
|   (o)    (o)    (o)    (o)                |  4 rotary encoders,
+------------------------------------------+  each with a push button
```

**The encoders have no LED rings.** Knob position/state is normally indicated on
the touch strip above them, not on the encoders themselves. `LBLIG` and `SETLB`
exist in the protocol but have no visible effect on this hardware — so there is
no cheap LED feedback path here, and every visible state change has to go
through the `BAT` image pipeline.

Build flag: `-D USERMOD_DECK_USB` (already set in the `esp32p4_8MB_troyhacks` env).

## Status: Phase 0 — transport bring-up

Phase 0 is implemented and builds clean. It exists to answer two questions that
cannot be answered from documentation, only from the physical device:

1. Does the P4 enumerate the CN003, and what does its descriptor look like?
2. What does an input report actually contain?

**Nothing above Phase 0 is written yet.** There is no `usermod_v3_deck.h` — no
action mapping, no feedback rendering, no key images. Input reports are logged to
the serial console and that is the whole behaviour. Do not treat this as a
working remote yet.

Note there is no cheap LED feedback path on this deck (no rings), so "Phase 3"
is a genuine image-push pipeline rather than a ring-colour tweak. Budget for it
accordingly — every knob or key state you want to show means encoding an image
and pushing it through `BAT`.

## Files

| File | Role |
|---|---|
| `deck_protocol.h` | Wire format. Packet builders and the inbound-report constants. Header-only, no I/O, no allocation. |
| `deck_usb_host.cpp` | The USB Host client. **Not a translation unit** — it is `#include`d into `wled.cpp` (see below). |

`deck_usb_host.cpp` is textually included from `wled00/wled.cpp` inside
`#ifdef USERMOD_DECK_USB`, exactly as `midi_usb_host.cpp` is. Every definition in
it is `static`. Consequences:

- It must **not** be added to any `build_src_filter`.
- Every file-scope symbol needs a `deck_` prefix. Both host clients land in the
  *same* translation unit, so two unprefixed `static` variables with the same
  name collide at compile time — this already happened once with
  `out_rb_storage` / `out_rb_struct` / `out_rb`.
- Do not introduce non-static symbols here.

## Protocol

Reconstructed from the [`mirajazz`](https://crates.io/crates/mirajazz) Rust crate
(a hardfork of `elgato-streamdeck`), which is what the OpenDeck plugin
`opendeck-akp05` links against. Mirabox/Ajazz devices do **not** use Elgato's
binary opcode scheme — every outbound packet is ASCII:

```
00 43 52 54 00 00 | <ASCII mnemonic> | <payload>
```

The mnemonic is variable length: `DIS`/`LIG`/`BAT`/`CLE`/`STP`/`HAN`/`MOD` are 3
bytes, `LBLIG`/`SETLB` are 5, `CONNECT` is 7.

| Opcode | Payload | Meaning |
|---|---|---|
| `DIS` | — | init / display reset; sent first on every connect |
| `LIG` | `00 00 <percent>` | keypad backlight brightness, 0–100 |
| `LBLIG` | `<percent>` | knob LED ring brightness — **no effect here, no rings** |
| `SETLB` | 3 bytes RGB per knob | per-knob ring colour — **no effect here, no rings** |
| `BAT` | `00 00 <len_hi> <len_lo> <key+1>` | image header; payload follows as 1024-byte chunks, each prefixed `0x00` |
| `CLE` | `00 00 <key+1>`, or `0xFF` for all | clear one key / clear all keys |
| `STP` | — | commit; without it a batch of images never appears |
| `CONNECT` | — | keepalive; the device drops the session without one roughly every 10 s |
| `HAN` | — | sleep |
| `MOD` | `00 00 <0x30+n>` | device mode |

Keys are indexed **from 1** on the wire (`key + 1`). Graceful shutdown is
`CLE 00 00 44 43` then `HAN`. The `44 43` argument is RE'd from a library that
assumed LED rings, so its visible effect on this hardware is **unverified** —
with no rings, the best guess is that it clears every key image, but confirm on
the bench before relying on it.

### Every outbound packet is 1025 bytes

Each command is **zero-padded to a fixed 1025-byte output report** before it
goes on the wire. `mirajazz` does this in `write_extended_data`, which resizes
every command buffer to `1 + packet_size` (1025 for protocol v2/v3) before
writing. The deck ignores a short packet.

This is why `deck_out_queue` pads centrally rather than each builder producing
its own length, and why the ring buffer holds fixed-size records. A bare 9-byte
`LIG` leaves the key backlight dark — which is exactly what the first bench
attempt showed.

### HID class bring-up — accepted by the device, but probably not what matters

The host sends `SET_PROTOCOL(Report)` and `SET_IDLE(0)` to the claimed interface
via `deck_hid_class_init()`, one request per pump of `deck_usb_poll()`. It uses
the async client because this IDF version has no synchronous clients
(`usb_host_client_config_t::is_synchronous` is documented "set to false for now"),
and this IDF's `usb_transfer_t` has no setup-packet field — the 8 SETUP bytes go
at the head of the data buffer and `num_bytes` includes them.

**Both return `OK` on hardware, and that rules out HID bring-up as the cause of
the silent IN pipe.** Note carefully what that does and does not prove: a device
can acknowledge these two requests and still not implement HID at all, which is
what the `GET_DESCRIPTOR(Report)` stall now suggests — see "What the stall means"
below. Do not treat their success as evidence that HID is in play.

### Inbound reports

512-byte interrupt IN reports. The layout is confirmed verbatim from
`DeviceStateReader::read_input` in `mirajazz/src/state.rs`:

| Offset | Meaning |
|---|---|
| `0..2` | `ACK` magic (`0x41 0x43 0x4B`); anything else is noise |
| `9` | **input** — a message ID, not a plain index |
| `10` | **state** — pressed / released |

The crate gates on `data.starts_with(&[65, 67, 75])` before touching `[9]` and
`[10]`, so those two bytes are meaningless unless the ACK check passes. There is
no `SET_IDLE`-style idle heartbeat: the crate simply reads.

The CN003 reports protocol v3, which sets `supports_both_keypress_states` and
`supports_both_encoder_states` to **false** — so `[10]` is not consulted at all
(the crate substitutes `0x01`), every keypress is a down+up pair, and encoder
press is edge-only with no press-and-hold on knobs.

### Input message IDs (from `opendeck-akp05/src/inputs.rs`)

| `[9]` | Meaning |
|---|---|
| `1`–`10` | the ten LCD keys |
| `0xa0`/`0xa1`, `0x50`/`0x51`, `0x90`/`0x91`, `0x70`/`0x71` | encoders 0–3, twisted (-1/+1) |
| `0x37`/`0x35`/`0x33`/`0x36` | encoder push-buttons 0–3 |
| `0x00`, `0x38`–`0x43` | further buttons with no display |

**None of this is decoded in the C++ yet** — Phase 0 only logs raw reports.

## Measured on hardware

From the first real enumeration of a CN003 on the P4:

| Question | Answer |
|---|---|
| VID / PID | `0x1500` / `0x3002` — Soomfon CN003, as assumed |
| Interface class | `0x03` (**HID**), subclass `0x00`, protocol `0x00` — *not* vendor-class `0xFF` |
| Deck interface | iface 0, 2 endpoints: **IN `0x82`, OUT `0x03`** |
| **Interrupt OUT endpoint exists** | **Yes** — `SETLB` LED control is viable, Phases 2 and 3 stand |
| Endpoint MPS | IN `0x82` = **512**, OUT `0x03` = **1024** (asymmetric — see the FIFO note below) |
| Second interface | iface 1, class `0x03` (HID) subclass `0x01` (boot) protocol `0x01` (keyboard), IN `0x81` only with **MPS 8** — that MPS-8 endpoint is the HID report-descriptor endpoint, not an input path |

The second interface is why `usermod_v3_midi` shows
`VID=0x1500 not in MIDI vendor list` and releases the device: it filters on a
vendor allow-list rather than on interface class. Adding Soomfon there would let
both clients coexist, but they would then race for the same device — leave it.

`IN_XFER_SIZE` is **512** — exactly the IN endpoint's MPS. ESP-IDF wants a
non-control transfer's `num_bytes` to be an integer multiple of MPS, and one full
maximum packet is the smallest value satisfying that.

### The 1024-byte MPS and the P4 FIFO split

The deck's endpoints are 1024 bytes, and the DWC host controller will not open a
pipe larger than its RX FIFO can hold. With this framework's stock sdkconfig
(`CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT`) the ceiling is 512, so
enumeration dies at claim time:

```
E (3857) HCD DWC: EP MPS (1024) exceeds supported limit (512)
E (3860) USBH: EP Alloc error: ESP_ERR_NOT_SUPPORTED
[DECK] open: interface_claim(iface=0) failed: ESP_ERR_NOT_SUPPORTED
```

This is a P4 host-controller FIFO sizing limit, not a deck or usermod bug. The
split is overridden in `wled00/wled.cpp` (`fifo_settings_custom`) — see the
comment there for the full table of what was tried.

## UNVERIFIED — confirm against real hardware

Everything above is RE'd from a Linux crate. **Treat the following as unknown
until Phase 0 has run against the deck:**

- **Which physical key each `[9]` value is.** The message-ID table is read off
  `opendeck-akp05`, and its `map_position` remaps raw values through an ordering
  that implies `0..3` are the four encoder push-buttons and `5..14` are the ten
  keypad positions (row 2 = `5..9`, row 1 = `10..14`), with `4` unused. That
  ordering is odd enough that it needs confirming before anything is built on it.
- **Which `rgb_swap` / `byte_swap` combination the JPEG path needs.** Not yet
  tried.

## Verifying on the bench

Build and flash, then watch the serial console:

```
pio run -e esp32p4_8MB_troyhacks -t upload
pio device monitor
```

Expected on plugging the deck in:

```
[DECK] event: NEW_DEV addr=2
[DECK] open: VID=0x1500 PID=0x3002 (Soomfon CN003) — claimed as deck
[DECK] open:   iface 0 class=0x03 subclass=0x00 protocol=0x00 eps=2 IN=0x82/1024 OUT=0x03/1024
[DECK] open: CLAIMED iface=0 IN=0x82 OUT=0x03
[DECK] open: 1 IN transfer submitted
[DECK] hid: SET_PROTOCOL(report) OK
[DECK] hid: SET_IDLE(0) OK
```

The two `hid:` lines are the ones to watch. A `stalled` here means the deck
refused the request and input may stay silent — that is the next thing to chase,
and the reason both lines log their status rather than staying quiet on failure.

(`addr=2` because the log shows the serial/CDC device already at `addr=1`.)

**Exit criteria — record the results here once measured:**

- [x] Deck enumerates and is claimed (above log appears) — confirmed on hardware.
- [x] Outbound packets are accepted — confirmed on hardware. The 1025-byte
      padding took effect: `out_done` climbs steadily and the deck now reacts to
      input. Before the padding the key backlight never came up at all.
- [x] `SET_PROTOCOL` and `SET_IDLE` both return `OK` — confirmed on hardware, and
      **this ruled out HID class bring-up as the cause of the silent IN pipe.**
      The requests are issued and accepted; input is still silent.
- [ ] Record the negotiated `speed=` from the open log — **confirmed `HIGH`,
      `ep0_mps=64`**, so the deck matches the forced high-speed PHY mode.
- [x] Deck is high-speed: `speed=HIGH`, `bcdUSB=2.00`.
- [ ] Watch for the watchdog lines. `IN: no report after 5 s — clearing pipe and
      re-arming (n/5)` five times, with `reports=` still 0, means the endpoint
      itself never delivers and re-arming is not the answer. Any `IN:` line at
      all means reports are flowing and the remaining work is the parser.
- [ ] `reports=` climbs when keys are pressed. A flat `reports=0` at idle is
      normal (interrupt IN NAKs when the device has nothing to say) but it must
      move under a keypress. **This is the open blocker** — see below.
- [ ] Every one of the 10 LCD keys produces an `ACK` report. Record which raw
      index each physical key uses: `________`.
- [ ] Each of the 4 encoders produces a twist report. Record the raw index and
      where the signed delta lives in the report body: `________`.
- [ ] Encoder push-button press produces a report: `________`.
- [ ] **Touch strip.** There is a full-width touch screen above the encoders
      that this transport has never considered. akp05 carries a
      `touch_image_format` (176×112 JPEG), so it is a real image surface — but
      whether its *input* arrives on this same HID interface (a report type we
      are currently discarding) or on an interface we are not claiming is still
      open. Until answered, assume there is no touch input and do not build a UI
      that depends on it: `________`.
- [ ] Encoder and key `state` is moot on this hardware — protocol v3 sets
      `supports_both_*_states` false, so `[10]` is ignored by the reference and
      everything is edge-only. Confirm nothing depends on it.

Set `DECK_DUMP_DESCRIPTORS` to 1 in `deck_usb_host.cpp` to dump the full
configuration descriptor — leave it off otherwise, as heavy logging inside the
brief `CONFIGURED` window has previously broken transfer delivery on this board.

### Open blocker: interrupt IN delivers nothing

Symptom, with everything else confirmed working: interface claimed, endpoint
addressed correctly, buffer sized to the MPS, `SET_PROTOCOL` and `SET_IDLE` both
accepted, OUT transfers completing steadily — and `reports=0` with the URB armed,
no matter how many keys and encoders are operated.

The device is not at fault. It demonstrably sees the presses itself: the screens
wake when a key is touched. The data is being produced and is going somewhere we
are not reading.

Leading suspect: **speed mismatch.** Ruled out — the open log reports
`speed=HIGH ep0_mps=64`, so the device is genuinely high-speed and matches the
forced `USB_PHY_SPEED_HIGH` PHY mode in `wled.cpp`.

Leading suspect, also ruled out on hardware: **endpoint/interface choice and idle
rate.** Both interrupt IN pipes the device offers are now claimed and armed, and
both are driven at a 10 Hz `SET_IDLE` heartbeat with the deck being actively
operated. Nothing arrives on either. See "Both IN pipes now tested" below.

Second suspect, **ruled out**: the IN URB used to be armed from inside the
client's event callback while the working OUT path was armed from
`deck_usb_poll()`. Both pipes are now armed from the main loop, and the endpoint
is still silent.

Also ruled out from the ESP-IDF v5.5 driver source, since the OUT pipe proves the
stack is functional in the directions that matter:

- **FIFO sizing.** `usb_dwc_hal_get_mps_limits` gives `in_mps = (rx_fifo_lines -
  2) * 4` = **1272** bytes for our `rx_fifo_lines = 320`, comfortably above the
  IN endpoint's 512. `non_periodic_out_mps` = 1024, `periodic_out_mps` = 1280,
  and `fifo_config_is_valid` passes at 320+256+320 = 896.
- **Periodic scheduling.** `pipe_set_ep_char` schedules *both* IN and OUT
  interrupt endpoints onto the periodic frame list, and `hal.c` writes the
  channel bit into it. The OUT endpoint already rides that scheduler and works.
- **Silent short packets.** `_buffer_parse_intr` treats `stop_idx > 0` on an IN
  transfer as a short-packet early stop and *completes* the transfer, so a short
  report would be reported, not dropped.

The distinguishing observation across all of this: **EP0 IN works** — enumeration
reads the device and config descriptors back over it, and my own probes re-read
both on demand — while the *periodic* IN endpoints deliver nothing. Non-periodic
IN is fine; periodic IN is the gap.

Alongside that, a bring-up watchdog re-arms a cleared pipe every 5 s until the
first report ever arrives. It ran all five permitted fires with `reports=` still
at 0, which answers its question: **the endpoint never delivers, and re-arming
is not the answer.** (The `status=3` callback that follows each fire is
`CANCELED` — the result of our own halt, not device data, and correctly not
counted as a report.)

#### Both IN pipes now tested — this closes off the obvious branches

Interface 1 / EP 0x81 had never been claimed or armed by any build. It is now
claimed as a *second, independently counted* input pipe (`AUX` in the status
line) rather than swapped in for the primary, so one boot answers the question:

```
open: CLAIMED iface=0 IN=0x82 OUT=0x03
open: AUX CLAIMED iface=1 IN=0x81 (boot-keyboard pipe)
hid: SET_IDLE(100ms,iface0) OK
hid: SET_IDLE(100ms,iface1) OK
status: ... reports=0 ... | AUX=0x81 reports=0 bytes=0 aux_in_flight=1
```

Interface 1 is a HID **boot-keyboard** collection (`bInterfaceSubClass=0x01`,
`bInterfaceProtocol=0x01`) with an 8-byte MPS and `bInterval=10`. It is armed
cleanly — `aux_in_flight=1` holds steady with no arm errors and no NAK churn.

Both interfaces were then put on a **100 ms SET_IDLE heartbeat**, so each is
asked to report at 10 Hz whether or not anyone touches the deck, and the run was
repeated with buttons pressed, encoders turned and the touch strip used. Final
state: `reports=0` and `AUX reports=0 bytes=0`.

That eliminates, on hardware rather than by argument:

- **wrong endpoint** — the second, entirely unexamined IN pipe is equally silent;
- **wrong interface** — both interfaces asked, both silent;
- **idle rate / on-change reporting** — a 10 Hz heartbeat on both, with real
  input, produces nothing;
- **HID class bring-up** — `SET_PROTOCOL` and per-interface `SET_IDLE` both
  accepted, and (probe B) EP0 still serves descriptors afterwards.

The device is alive and talking to us on EP0 and on the interrupt OUT pipe; it
does not send on any interrupt IN pipe it offers. This is now a genuine hardware
or firmware-level difference between the OUT and IN paths on this deck, and it
is not reachable from the host side by any of the levers available in Phase 0.

**Next step, not yet tried:** a USB bus analyzer / hardware capture of EP 0x82 to
settle whether the device ever puts a packet on that IN line at all. Every
host-side hypothesis has now been exhausted, and the remaining ambiguity is
whether the deck transmits input that we are failing to see, or whether it
transmits nothing — which only a bus capture can distinguish from software logs.
`deck_dump_raw_descriptors()` is the pattern to copy for that: stop inferring
from the host's view and go look at the wire.

**Protocol gating is also off the table.** Every opcode `mirajazz` can emit was
enumerated from `device.rs` — `DIS`, `LIG`, `LBLIG`, `SETLB`, `BAT`, `CLE`,
`STP`, `HAN`, `MOD`, `CONNECT`, `QUCMD` — and the open sequence is only
`initialize()`, which sends `DIS` then `LIG 0`. We send `DIS`, `LIG`, `STP` and
a `CONNECT` keepalive, which covers it. There is no "enable input" command being
missed. Relatedly, `"ACK"` is the *report-type* magic read by `inIsAck`, not a
command acknowledgement, so the absence of inbound data is not evidence of a
refusal to talk.


Since guessing has stopped paying, the next step is to read the answer off the
wire instead: `GET_DESCRIPTOR(Report)` on the claimed interface. The
configuration descriptor only says the pipe exists; the report descriptor is the
authoritative statement of what the device can send, how large a report is, and
whether there is a report-ID prefix. `deck_hid_class_init()` fetches it after
`SET_PROTOCOL`/`SET_IDLE` and logs the length, the first 24 bytes and the
leading item's bTag/bType/bSize.

**This request stalls, and the stall is itself the finding.** The device accepts
`SET_PROTOCOL` and `SET_IDLE` — both return `OK` — but STALLs a
`GET_DESCRIPTOR(Report)` that every conformant HID device must answer, and the
stack logs `USBH: Dev 2 EP 0 STALL`. So interface 0 declares `bInterfaceClass
0x03` but does not behave like a real HID interface. `bInterfaceClass` is
therefore not a reliable statement about which interface carries input on this
firmware, and the interface walk no longer uses it to filter candidates — any
interface with an interrupt IN endpoint is now a candidate, with class used only
to rank. See "Descriptor walk" below.

### This IS a HID device — but two of my own probes were malformed

Corrected. An earlier section of this README concluded the device "is not a HID
device" and that the `bcdHID` byte was decorative. **That was wrong**, and
`deck_dump_raw_descriptors()` — added to test it — disproved it on the hardware:

```
dump: config descriptor 66 bytes, bNumInterfaces=2
dump: @ 18 type=0x21 len=9  09 21 00 02 00 01 22 36 00
dump: @ 50 class type=0x21 len=9  09 21 00 02 00 01 22 3F 00
dump:     HID bcdHID=0x0200 bCountryCode=00 bNumDescriptors=1
dump:     class desc type=0x22 len=63
```

Both interfaces carry `bcdHID=0x0200`, `bNumDescriptors=1`, and a genuine type
`0x22` report descriptor — 54 bytes on interface 0, 63 on interface 1. The
"NO trailing class descriptor" branch never fired. This is ordinary HID
declaring a vendor-defined collection.

The reference agrees, and should have been weighted more heavily before I
asserted otherwise. `mirajazz` selects the device with
`DeviceQuery::new(65440, 1, 0x1500, 0x3002)` — usage page `0xFF00`, usage `1`.
A **vendor usage page** is the normal way this class of device is exposed; it is
HID, not an alternative to HID.

Arguments I made that were bad, and should not be repeated:

- *"The `ACK` framing isn't HID-shaped."* A HID report descriptor can declare any
  byte layout at all, including literal ASCII constants. This was a weak argument
  presented as load-bearing.
- *A STALL proves the device isn't conformant.* Cheap firmware stalls for reasons
  unrelated to conformance.
- *"The stall was my oversized `wLength`."* Plausible, and it was wrong. Asking
  for exactly the declared 54 bytes stalls identically. That was an earlier
  version of the same mistake — explaining a symptom away instead of testing it.

#### The real error: bmRequestType 0x81 on a device-recipient request

The one stall that mattered came from a bug in my own probe. `bmRequestType` is
`direction | type | recipient`, and the low bit is the **recipient**:

| Code | Meaning | Correct for |
|------|---------|-------------|
| `0x80` | device-to-host, standard, **device** | `GET_DESCRIPTOR(Device)` |
| `0x80` | device-to-host, standard, **device** | `GET_DESCRIPTOR(Configuration)` |
| `0x81` | device-to-host, standard, **interface** | only interface-class requests |
| `0x81` | device-to-host, **class**, **interface** | `GET_DESCRIPTOR(Report)` |

I sent `0x81` for all three, i.e. I was asking the *interface* for the *device*
descriptor — a malformed request the deck was entirely entitled to refuse, and I
read the resulting stalls as evidence about the device across several runs.

With the correct `0x80`, both standard fetches succeed:

```
hid: A:GET_DESCRIPTOR(Device) OK   -> 12 01 00 02 00 00 00 40 00 15 02 30 02 00 01 02 03 01
hid: A:GET_DESCRIPTOR(Config) OK   -> 66 bytes, identical to deck_dump_raw_descriptors()
hid: B:GET_DESCRIPTOR(Device) OK   -> still fine AFTER SET_PROTOCOL + SET_IDLE
hid: B:GET_DESCRIPTOR(Report)      -> stalled
```

Probe A runs before any class request, probe B after. B succeeding is a separate
negative result: the HID class writes do **not** poison EP0.

What survives is one narrow fact: **`GET_DESCRIPTOR(Report)` stalls**, at the
correctly addressed `0x81`, with `wLength` exactly equal to the 54 bytes the
device itself advertises. Device and Config descriptors are served normally.
That single refusal is worth naming precisely, because it is the one thing a
Linux HID driver cannot work around — it needs the report descriptor to learn
usage page and usage, which is how `opendeck-akp05` selects this device at all.

### The host-side search is over — Phase 0 cannot close this

Recorded here because the *method* matters as much as the result: every
remaining hypothesis was excluded by a bench run, not by argument.

| # | Hypothesis | Test | Result |
|---|-----------|------|--------|
| 1 | Not a HID device | raw descriptor dump | **Wrong** — real `bcdHID=0x0200`, real `0x22` |
| 2 | Oversized `wLength` on the Report fetch | ask for exactly 54 | **Wrong** — identical stall |
| 3 | Device refuses IN data stages | `GET_DESCRIPTOR(Device)` | **Wrong** — serves it fine |
| 4 | HID class requests poison EP0 | probe A before / probe B after | **Wrong** — B succeeds |
| 5 | Wrong endpoint | claim + arm iface 1 / EP 0x81 | **Wrong** — equally silent |
| 6 | Wrong interface | `SET_IDLE` per interface, both | **Wrong** — both silent |
| 7 | Idle rate / on-change reporting | 10 Hz heartbeat + button presses | **Wrong** — nothing |
| 8 | Wrong init sequence | `DIS` + `LIG 0` only, byte-for-byte as mirajazz | **Wrong** — nothing |
| 9 | Read armed before device init | defer IN pipes until `DIS`/`LIG` drained | **Wrong** — nothing |

Hypotheses 3 and 4 were my *own* bugs, not the device's — see the `bmRequestType`
note above. That is the main methodological lesson from this phase: twice the
symptom I was explaining was a fault in the probe. Two of the three original
"the deck is unusual" conclusions were mine, and the deck was correct both times.

The final run, with buttons pressed throughout:

```
3253  status: ... in_flight=0 ... aux_in_flight=0     <- deliberately NOT armed
3390  hid: B:GET_DESCRIPTOR(Report) -> stalled
3393  init: DIS/LIG sent (out_done=2), HID setup done — arming IN pipes
3403  open: 1 IN transfer submitted
3403  open: AUX 0x81 1 transfer submitted
36253 status: ... reports=0 ... | AUX=0x81 reports=0 bytes=0
```

`out_done=2` confirms the wire carried `DIS` + `LIG 0` and nothing else, so the
reference's init sequence was tested exactly. `in_flight=0` at claim confirms the
pipes were held until the device was initialized, so the reference's *ordering*
was tested exactly. Both IN pipes then stayed armed and silent for 36 s of
active button input.

**What remains, and why it needs hardware:** the only distinction still
available is between "the deck transmits input we fail to see" and "the deck
transmits nothing." No host-side log can separate those — our view of the IN
pipe is exactly what is in question. A USB bus analyzer / protocol capture on
EP 0x82 settles it in seconds: a decoded `SOF`/`DATA`/`NAK` pattern shows
whether the device ever puts a packet on that line.

Two further signals in the log that are worth keeping:

- **The aux pipe is the healthiest thing here.** `aux_in_flight=1` holds for the
  full 36 s with no arm failure and no NAK churn, while 0x82 is simultaneously
  failing to re-arm. So the DWC2 stack handles one interrupt IN pipe cleanly —
  "periodic IN is broken on this host" is not the story either.
- **`IN arm failed: ESP_ERR_NOT_FINISHED`, 5 for 5.** This is *our* halt racing
  the async cancel, not device behaviour: the recovery path is genuinely broken
  and should be fixed before Phase 1 regardless of what the capture shows.

### Why the stall was never the cause of the silent IN pipe

Worth keeping separate, because the two were conflated. The first `status:`
line, printed at boot, already reads `reports=0 in_flight=1`, and the EP0 stall
is timestamped ~4 s later. The pipe was silent from the start; the control-layer
stall came after and cannot explain it.

### Descriptor walk

Earlier versions of the walk filtered on interface class *before* logging, so an
interface of any other class — including one holding the real input endpoint, if
one exists — was silently invisible in the open log. That was a blind spot in the
only evidence available, so the walk now logs **every** interface and **every**
endpoint, with transfer type, MPS and `bInterval`, regardless of class. Measured
on hardware, the device has exactly two interfaces and nothing was hidden:

```
[DECK] open:   iface 0 class=0x03 subclass=0x00 protocol=0x00 eps=2 alt=0 IN=0x82/512 OUT=0x03/1024 bcdHID=present
[DECK] open:     ep 0x82 INTR mps=512 interval=1
[DECK] open:     ep 0x03 INTR mps=1024 interval=1
[DECK] open:   iface 1 class=0x03 subclass=0x01 protocol=0x01 eps=1 alt=0 IN=0x81/8 OUT=0x00/0 bcdHID=present
[DECK] open:     ep 0x81 INTR mps=8 interval=10
```

Interface 1 is a boot keyboard (`subclass 0x01`, `protocol 0x01`) with an 8-byte
MPS — a standard HID keyboard endpoint, not a deck input path. This dump also
**ruled out** the interface-selection theory outright: there is no third
interface, and interface 1 cannot carry the 512-byte reports the crate reads.

This is a permanent change, not a `DECK_DUMP_DESCRIPTORS` debug flag: it runs
once per enumeration, costs a few lines, and it is the only remaining source of
truth now that six hypotheses have been ruled out.

## Relationship to usermod_v3_midi

`usermod_v3_midi` is the template for this: same USB host client shape, same
per-control dedup slots, same `onStateChange` feedback hook, same two-press
confirmation for destructive actions. Both clients register with `usb_host_lib`
simultaneously and each has a VID/PID filter, so they do not fight over a device.

Only one control surface can be plugged into the port at a time, and an SD card
(MSC) is mutually exclusive with either host client on this IDF state.

## Session handoff — open questions and next steps

Written 2026-10-20, after a review pass over the whole Phase 0 investigation.
This section records what is known, what is contaminated, and what to do first.

### Watchdog removed

The IN bring-up watchdog has been **deleted**, along with `IN_WATCHDOG_MS`,
`IN_WATCHDOG_MAX` and `in_watchdog_fires`. `last_in_completed_ms` remains, as it
is only diagnostic.

This is the single most important change made in the review pass, because the
watchdog **destroyed the evidence it was meant to gather**:

```cpp
// REMOVED — do not reintroduce
if (deck_dev_hdl && deck_in_xfer && deck_in_completed == 0 &&
    in_flight && !in_resubmit_needed && !in_arm_pending &&
    in_watchdog_fires < IN_WATCHDOG_MAX &&
    (uint32_t)(millis() - last_in_completed_ms) >= IN_WATCHDOG_MS) {
  ...
  deck_in_arm(true);
}
```

The condition requires `in_flight` — a URB is armed — and then calls
`deck_in_arm(true)`, whose first action with a URB out is
`usb_host_endpoint_halt()`. **That cancels the in-flight URB.** The callback then
reports `CANCELED`, which sets `in_error_recovery_needed`, and the next poll
re-arms with `clear_first = true` — a flush plus clear, which **resets the data
toggle**.

So for the first 25 seconds the primary IN pipe was halted, flushed and
re-armed five times. A report arriving at t=6 s was destroyed by the t=10 s
teardown; one arriving at t=21 s was destroyed at t=25 s. The only reports that
could ever have been counted are ones that completed inside an undisturbed 5 s
window — and whether anything arrived during the *first* window was never
isolated.

Therefore: **"the endpoint never delivers" is not established.** It is an
inference drawn from a window in which the pipe was being torn down five times by
our own code.

`deck_in_arm(true)` is now called from nowhere. `clear_first` is only ever true
after a genuinely failed transfer (`in_error_recovery_needed`), which is the
correct behaviour: recover from a real failure, never preemptively.

Two related consequences worth carrying forward:

- The README's earlier observation that "the aux pipe is the healthiest thing
  here, `aux_in_flight=1` holds for the full 36 s" was itself the tell. Nothing
  ever disturbed EP 0x81, while EP 0x82 was being sabotaged every 5 s. The
  experiment conflated *which pipe* with *which pipe is being torn down*.
- `IN arm failed: ESP_ERR_NOT_FINISHED` 5-for-5 is not an independent finding.
  It is the watchdog's own halt, counted twice.

### Second-order reasoning to be careful about

`aux_in_flight=1` proves the pipe *holds*, not that data *flows*. EP 0x81 is an
8-byte boot-keyboard endpoint; if the deck maps nothing to it as a keyboard, it
will stay silent forever. Its silence says nothing about EP 0x82, and its
steadiness does not rule out periodic-IN being broken on the host.

### Closed by re-reading the reference — no hardware needed

`opendeck-akp05/src/inputs.rs` answers two questions this file previously listed
as open, for free:

1. **Encoder direction is not a body byte.** `read_encoder_value` maps message
   IDs to ±1, so the direction is encoded in the message ID itself and the device
   emits one message per detent:
   `0xa0/0xa1` = encoder 0, `0x50/0x51` = encoder 1, `0x90/0x91` = encoder 2,
   `0x70/0x71` = encoder 3.
2. **Each report carries a full button bitmap, not a delta.**
   `read_button_states` reads `states[i+1] != 0` across a 16-byte array built as
   `[0x01] + [0u8; KEY_COUNT + 1]`, with `button_states[pressed_index] = state`.
   So bytes ~11–27 of the report should be that 16-entry array. Check this the
   first time a report lands — it is the cheapest confirmation that input is
   real.

Also note each message is an **absolute state snapshot**; `mirajazz`'s
`input_to_updates` diffs consecutive snapshots to synthesise Down/Up. Our
Phase 1 parser should do the same rather than treating each report as an edge.

### The highest-value next step: get the report descriptor

The HID report descriptor was never obtained, and it was available the whole time
**on the desktop**. The deck demonstrably enumerates on Linux via OpenDeck, and
Linux reads that descriptor over the *same* control transfer at enumeration. It
would have settled report size, report-ID prefix and input structure from day
one. On the machine where OpenDeck works:

```
lsusb -v -d 1500:3002
sudo cat /sys/kernel/hidraw/*/report_descriptor
```

Two days of host-stack archaeology went into questions the kernel already
answered. Do this before any further host-side work.

Note the implication for the one stall that survived review: Linux must succeed
at `GET_DESCRIPTOR(Report)` for `async_hid` to enumerate this device at all, so
if our probe still stalls at the correct `bmRequestType` (0x81) and the correct
`wIndex`, the probe is probably *still* wrong rather than the device being
unusual. (`wIndex` is set correctly as of this review — `deck_usb_host.cpp:670`.)

### Next steps, in order

1. **Flash and run with the watchdog gone.** Press keys and turn encoders from
   t=0. This is a five-minute experiment and it is the first thing that must
   happen. Either input flows and the blocker is gone, or it does not and the
   conclusion is finally clean.
2. **Dump the report descriptor on Linux** and paste it into this file. Closes
   the remaining unknowns and probably the blocker itself.
3. **Only then consider Phase 1.** There is no value in building a parser on an
   unverified byte layout while the pipe is possibly closed.

### Still unverified

- **The 1025-byte padding of every outbound command.** The only evidence is
  `out_done` climbing steadily, which proves transfers *completed*, not that the
  device *parsed* them. It is harmless for now but it caps the 16 KB OUT ring at
  16 commands, and a 15-key image push will need far more than that in Phase 3.
  If Phase 3 hits a throughput wall, suspect this first — it may be a
  cargo-culted reading of `mirajazz`'s `write_extended_data`, where the 1025
  figure is the *image chunk* size rather than a universal command size.
- **The FIFO override was not re-verified against the Akai APC Mini MK2.**
  `rx_fifo_lines` was reduced 512 → 320 and `ptx` raised 128 → 320 in
  `wled00/wled.cpp`. That is a **global** host setting shared with MSC and the
  MIDI controller, in a setup that took many iterations to stabilise. The change
  is genuinely necessary — `interface_claim` fails outright with `EP MPS (1024)
  exceeds supported limit (512)`, because an interrupt OUT pipe is bounded by the
  periodic TX FIFO and balanced sdkconfig provides only 128 lines = 512 B, so
  the deck's interface cannot be claimed at all without it. But plug the APC
  back in once and confirm it still enumerates before trusting this.
- **`rgb_swap` / `byte_swap` combination for the JPEG path.** Not yet tried.
- **Touch strip input.** A full-width touch surface above the encoders that this
  transport has never considered. `akp05` carries a `touch_image_format`
  (176×112 JPEG), so it is a real image surface, but whether its input arrives
  on this HID interface or on one we do not claim is still open. Assume there is
  no touch input; do not build a UI that depends on it.
- **Whether the encoders have LED rings.** The README asserts they do not, based
  on the observed absence of any visible response to `SETLB` / `LBLIG`. If that
  is wrong it materially changes Phase 2, so confirm by eye on the bench.

### What was done well and should not be undone

- **The FIFO analysis.** Real, necessary, and correctly reasoned from the DWC
  limits. Kept, with the APC re-check noted above.
- **The `bmRequestType` self-correction.** `0x81` was being sent for
  device-recipient requests, and the resulting stalls were then read as evidence
  *about the device* across several runs. Catching that, and labelling the
  earlier conclusion wrong in this file rather than quietly editing it, is the
  right instinct and is why the remaining stall is credible enough to chase.
- **The descriptor walk now logging every interface and endpoint** regardless of
  class. An earlier version filtered before logging, so an interface of any other
  class was invisible in the only evidence available. Permanent, not a debug flag
  — correct call.
- **The phase discipline.** Nothing above Phase 0 was written early, so there is
  no half-built usermod class to unpick.

### The methodological lesson

Recorded because it generalises beyond this bug: **twice here, the fault was in
the probe, not the device.** A malformed `bmRequestType` read as device
unusualness; oversized `wLength` explained away as a symptom rather than tested.
The third instance is the watchdog — not a wrong argument this time, but our own
code corrupting the measurement. Before concluding a device misbehaves, rule
out that the measurement itself is the thing at fault.

### Build configuration changed in the "more work" commit

Worth knowing so the next person does not misread the size numbers. That commit
also changed `platformio_override.ini` beyond the deck itself:

- `-flto` **disabled**, and `pio-scripts/enable_lto.py` removed from
  `extra_scripts`.
- `WLEDMM_DISPLAY_MODE=P4_1280x720_60HZ` **disabled**.
- `ARDUINO_USB_MODE=1` and `ARDUINO_USB_CDC_ON_BOOT=1` **disabled**.
- `REALTIME_OUTPUT_TIMER` **disabled**.

Effect on the reported figures: RAM fell from ~14.7% to ~9.1% (the 1280×720
framebuffer is gone) while flash rose from ~67.9% to ~69.6% (no LTO). A later
build with the watchdog removed measured **RAM 9.1% (46716 B), flash 69.6%
(2327098 B)** — build clean.

The `ARDUINO_USB_*` lines differ per P4 board in use — not a gotcha, just board
configuration.

LTO is disabled purely to make builds faster, not for any reason to do with the
deck. Re-enabling it should be safe: `deck_usb_host.cpp` is `#include`d into
`wled.cpp`, so its `static` symbols live in a single translation unit and
cross-TU `--gc-sections` stripping — the failure mode recorded in "Files"
above — cannot apply to them regardless of LTO.

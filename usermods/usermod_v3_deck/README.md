# usermod_v3_deck — Soomfon CN003 as a WLED control surface

ESP32-P4 running WLED acts as the USB **host** for a Soomfon CN003 / Mirabox
N4-family stream deck. No PC, no OpenDeck, no emulation: the P4 enumerates the
deck, reads its keys and rotary encoders, drives WLED state, and paints the deck's
LCD keys and knob LED rings.

Build flag: `-D USERMOD_DECK_USB` (already set in the `esp32p4_8MB_troyhacks` env).

## Status: Phase 0 — transport bring-up

Phase 0 is implemented and builds clean. It exists to answer two questions that
cannot be answered from documentation, only from the physical device:

1. Does the P4 enumerate the CN003, and what does its descriptor look like?
2. What does an input report actually contain?

**Nothing above Phase 0 is written yet.** There is no `usermod_v3_deck.h` — no
action mapping, no LED feedback, no key images. Input reports are logged to the
serial console and that is the whole behaviour. Do not treat this as a working
remote yet.

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
| `LBLIG` | `<percent>` | knob LED ring brightness, 0–100 |
| `SETLB` | 3 bytes RGB per knob | per-knob ring colour — the cheap feedback path |
| `BAT` | `00 00 <len_hi> <len_lo> <key+1>` | image header; payload follows as 1024-byte chunks, each prefixed `0x00` |
| `CLE` | `00 00 <key+1>`, or `0xFF` for all | clear one key / clear all keys |
| `STP` | — | commit; without it a batch of images never appears |
| `CONNECT` | — | keepalive; the device drops the session without one roughly every 10 s |
| `HAN` | — | sleep |
| `MOD` | `00 00 <0x30+n>` | device mode |

Keys are indexed **from 1** on the wire (`key + 1`). Graceful shutdown is
`CLE 00 00 44 43` then `HAN`, which leaves all knobs solid red.

### Inbound reports

512-byte interrupt IN reports:

| Offset | Meaning |
|---|---|
| `0..2` | `ACK` magic (`0x41 0x43 0x4B`); anything else is noise |
| `9` | index — which key or encoder changed |
| `10` | state — pressed / released |

Encoder rotation arrives as a signed `i8` delta. The Rust crate reports
`supports_both_encoder_states() == false` for this family, so encoder *press* is
edge-only — Down and Up arrive back to back. No press-and-hold on knobs.

## UNVERIFIED — confirm against real hardware

Everything above is RE'd from a Linux crate. **Treat the following as unknown
until Phase 0 has run against the deck:**

- **The key index map.** The OpenDeck plugin's `map_position` implies raw index
  `0..3` are the four knob press-buttons and `5..14` are the ten keypad
  positions (row 2 = `5..9`, row 1 = `10..14`), with `4` unused. That ordering is
  odd enough that it needs confirming before anything is built on it.
- **Interface class.** The walk accepts both vendor-class (`0xFF`) and HID
  (`0x03`) interfaces and scores candidates (vendor + bidirectional wins), because
  the firmware may declare either. The log will show which one was claimed.
- **Endpoint addresses and whether an interrupt OUT endpoint exists at all.**
  `SETLB` LED control is untested — if there is no OUT endpoint the device is
  input-only and Phases 2 and 3 need rethinking.
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
[DECK] event: NEW_DEV addr=1
[DECK] open: VID=0x1500 PID=0x3002 (Soomfon CN003) — claimed as deck
[DECK] open:   iface 0 class=0xFF subclass=0x00 protocol=0x00 eps=2 IN=0x81 OUT=0x01
[DECK] open: CLAIMED iface=0 IN=0x81 OUT=0x01
[DECK] open: 1 IN transfer submitted
```

**Exit criteria — record the results here once measured:**

- [ ] Deck enumerates and is claimed (above log appears).
- [ ] Knob rings light red / green / blue / yellow on connect, one per knob.
- [ ] Every one of the 15 keys produces an `ACK` report. Record which raw index
      each physical key uses: `________`.
- [ ] Every one of the 4 knobs produces a twist report. Record the raw index and
      where the signed delta lives in the report body: `________`.
- [ ] Knob press produces a report: `________`.
- [ ] Note whether state `0x01` means down or up.

Set `DECK_DUMP_DESCRIPTORS` to 1 in `deck_usb_host.cpp` to dump the full
configuration descriptor — leave it off otherwise, as heavy logging inside the
brief `CONFIGURED` window has previously broken transfer delivery on this board.

## Relationship to usermod_v3_midi

`usermod_v3_midi` is the template for this: same USB host client shape, same
per-control dedup slots, same `onStateChange` feedback hook, same two-press
confirmation for destructive actions. Both clients register with `usb_host_lib`
simultaneously and each has a VID/PID filter, so they do not fight over a device.

Only one control surface can be plugged into the port at a time, and an SD card
(MSC) is mutually exclusive with either host client on this IDF state.

# usermod_v3_hidctl — generic USB HID control surface

Use any USB HID controller as a WLED control surface on the ESP32-P4.

The P4's USB host stack claims the controller's HID interface (USB class 3)
and reads its input reports. What the reports *mean* comes from a **profile**:
a JSON file, one per controller, in the LittleFS root. Profiles are not part
of `cfg.json` — they are separate files, so a controller can be added,
changed or removed without touching the runtime configuration.

The **Traktor Kontrol F1** profile ships with the firmware and is written to
`/hidctl-traktor-kontrol-f1.json` on first boot.

## Build

```
-D USERMOD_HIDCTL_USB
```

in the P4 env (`esp32p4_8MB_troyhacks` in `platformio_override.ini`).

## Profile format

```json
{
  "name": "Traktor Kontrol F1",
  "vid": "0x17CC",
  "pid": "0x1120",
  "report_id": 1,
  "report_len": 22,
  "buttons": { "bytes": [1, 2, 3, 4], "msb_first": true },
  "controls": [
    { "name": "knob1",  "offset": 6,  "bits": 12, "action": "bri" },
    { "name": "knob2",  "offset": 8,  "bits": 12, "action": "effectSpeed" },
    { "name": "knob3",  "offset": 10, "bits": 12, "action": "effectIntensity" },
    { "name": "knob4",  "offset": 12, "bits": 12, "action": "effectPalette" },
    { "name": "fader1", "offset": 14, "bits": 12, "action": "seg0bri" },
    { "name": "fader2", "offset": 16, "bits": 12, "action": "seg1bri" },
    { "name": "fader3", "offset": 18, "bits": 12, "action": "seg2bri" },
    { "name": "fader4", "offset": 20, "bits": 12, "action": "seg3bri" }
  ],
  "button_map": [
    { "index": 0, "name": "pad1", "action": "preset1", "mod_action": "preset17" },
    { "index": 16, "name": "shift", "modifier": true },
    { "index": 28, "name": "sync", "action": "nextfx" }
  ],
  "output": {
    "report_id": 128,
    "length": 81,
    "display": {
      "digits": [ { "offset": 9 }, { "offset": 1 } ],
      "dot_offset": 8,
      "segments_per_digit": 7,
      "segments": ["middle", "lower-right", "upper-right", "top",
                   "upper-left", "lower-left", "bottom"]
    },
    "lights": [
      { "name": "browse", "offset": 17, "bytes": 1 },
      { "name": "pad1",   "offset": 25, "bytes": 3 }
    ]
  }
}
```

| Key | Meaning |
|-----|---------|
| `name` | shown in the info page |
| `vid` / `pid` | USB IDs to match (`0x` prefix optional) |
| `report_id` | only accept reports with this ID byte; omit to accept any |
| `report_len` | bytes in the report, including the ID byte |
| `buttons.bytes` | which report bytes hold the button bitmaps |
| `buttons.msb_first` | `true` = bit 7 of the first byte is button 0 |
| `controls[].offset` | byte offset of the value in the report |
| `controls[].bits` | width of the value (little-endian) |
| `controls[].action` | what the control does in WLED |
| `button_map[].index` | global button index (bytes concatenated, MSB first) |
| `button_map[].action` | what a plain press does |
| `button_map[].mod_action` | what a press does while a modifier is held |
| `button_map[].modifier` | `true` for modifier keys (SHIFT) |
| `output.report_id` | the report ID of the LED report |
| `output.length` | its length, including the ID byte |
| `output.display.digits[].offset` | where each digit's strokes start |
| `output.display.dot_offset` | the byte of the dot between the digits |
| `output.display.segments` | the stroke names, in the order they appear |
| `output.lights[].offset` | where the light's bytes start |
| `output.lights[].bytes` | 1 = one brightness, 2 = two, 3 = blue, red, green |

## Action vocabulary

| Group | Actions |
|-------|---------|
| parameters | `bri`, `effectSpeed`, `effectIntensity`, `effectPalette`, `effectCustom1`, `effectCustom2`, `seg0bri`..`seg7bri` |
| verbs | `power`, `blackout`, `full`, `nextfx`, `prevfx`, `nextpal`, `prevpal`, `nextpreset`, `prevpreset`, `toggleFreeze`, `seg0`..`seg7`, `downbeat`, `cleardownbeat`, `relock` |
| presets | `preset1`..`preset64`, `savepreset1`..`savepreset64` |

On the F1 the 16 pads are presets 1..16 and SHIFT + a pad is presets
17..32.

### The F1's pad colours and the beat counter

The pads show the state of their preset, in the same categories the APC
Mini's pads use:

| State | Colour |
|-------|--------|
| the active preset | green |
| a saved preset | blue |
| a saved looping playlist | magenta |
| a saved one-shot playlist | red |
| the music playlist | yellow |
| not saved | dark |

That comes out of the core's preset cache (the same metadata the MIDI
usermod's pads use).

The four lower buttons are the **beat counter**: the beat that is sounding
is lit, taken from the tempo tracker's published frame. The first of them
**sets the downbeat** — the same request the serial console's `j` makes —
and SHIFT with them still selects the segment.

A parameter action is driven by a continuous control: the control's value is
scaled to 0..255 and applied. A verb runs once per press.

**Soft takeover**: a continuous control has to cross the parameter's current
value before it starts driving it, so a knob that sits somewhere else does
not yank the value when you first touch it.

## The Traktor Kontrol F1

The F1 is a HID device (class 3) on NI's vendor usage page 0xFF01, with a
DFU interface alongside. It is **not** a class-compliant MIDI device — NI's
own Windows driver turns the HID reports into MIDI ("Kontrol F1 Midi"), and
only in the controller's MIDI mode. On the P4 the HID reports are read
directly, so no NI driver is involved.

Its 22-byte input report (report ID 0x01):

```
byte  0       report ID
bytes 1..4    button bitmaps (bit 7 of byte 1 is button 0)
byte  5       status byte (counts up while the device is awake)
bytes 6..21   eight 12-bit little-endian values
```

A 12-bit value at offset `off` is `b[off] | ((b[off+1] & 0x0F) << 8)`.

The eight continuous controls are the four FILTER knobs (centre detent at
~2048) followed by the four channel faders (0 at the bottom) — the detent is
what tells the two groups apart.

The button map was established by capturing the raw reports while each
physical control was pressed (see `tools/f1_hid_probe.py`,
`tools/f1_hid_buttons.py` and `tools/f1_hid_analyze2.py` in this repo):

| Index | Control | Index | Control |
|-------|---------|-------|---------|
| 0..15 | the 16 pads, top-left first | 21 | the browser encoder (push) |
| 16 | SHIFT | 22, 23 | unused |
| 17 | REVERSE | 24..27 | the four lower buttons |
| 18 | TYPE | 28 | SYNC |
| 19 | SIZE | 29 | QUANT |
| 20 | BROWSE | 30 | CAPTURE |

Default mapping: the knobs drive brightness / speed / intensity / palette,
the faders drive the brightness of segments 0..3, the pads load presets 1..16
(SHIFT + pad saves the preset), and the named buttons navigate effects,
palettes and presets.

**One mapping is not confirmed by the captures**: the order of the four lower
buttons (indices 24..27) across the row. The capture that swept them left to
right pressed 24, 25, 26, 27 in that order, so that is what the profile uses
— but if it turns out to be reversed, it is a one-line change in the profile
file, no rebuild.

### The LED demo

The controller can be put into a demo pattern — the transport buttons
chase, the lower buttons sweep, the pads turn a rainbow and the display
counts — without a rebuild:

```
curl -X POST http://<board>/json/state -d '{"HIDCTL":{"demo":true}}'
```

or with the serial console's `r` key (it toggles). The state is in
`/json/ar` under `HIDCTL.demo`.

It is also how the brightness was measured: the transport row was given
eight different levels and the lower buttons a gradient across each one's
two halves. **The buttons take a brightness (00 to 7F), not an on/off
code, and there is no flash code** — a byte above the maximum does
nothing at all. Any flashing is the host's job: pulse the brightness.

## LED feedback

The F1's pads, buttons and two-digit display are driven by an 81-byte
report (ID 0x80) that goes out on the interface's **interrupt OUT endpoint**
(0x01) — *not* over the control pipe. The device stalls every HID class
request: SET_REPORT with bRequest 0x09 is recognised (it stalls on the
report rather than refusing the request code), but no report ID and length
in the descriptor is accepted, and the other request codes get
"invalid function" outright. The OUT endpoint is the only way in.

The report (the ID byte first, then the data):

| Byte(s) | What |
|----------|------|
| 0 | report ID, 0x80 |
| 1–7 | the right-hand digit, seven strokes |
| 8 | the dot between the digits |
| 9–15 | the left-hand digit, seven strokes |
| 16 | not identified |
| 17–24 | Browse, Size, Type, Reverse, Shift, Capture, Quant, Sync (brightness) |
| 25–72 | the 16 pads, three bytes each: **blue, red, green** |
| 73–80 | the four lower buttons, two bytes each (left, right), in the order 4, 3, 2, 1 |

Every byte is a brightness from 00 to 7F. The transport buttons and the
lower four have fixed colours on the hardware (Browse is blue, Shift is
white, the rest orange), so their byte only turns them on and off.

This was confirmed against the hardware: the pad colour order came out as
blue, red, green, and Sync stayed dark because its byte is the eighth of
the transport row.

Sources: the [Mixxx wiki](https://github.com/mixxxdj/mixxx/wiki/Native-Instruments-Traktor-Kontrol-F1)
for the layout, [NexAdn's reverse engineering](https://github.com/NexAdn/ni-traktor-kontrol-f1)
for the report, and the device's own HID report descriptor (fetched over the
USB host stack) for what the interface declares.

### The encoder's rotation

The encoder's position is byte 5 of the input report: it counts up one
step per detent clockwise and down anticlockwise, wrapping at 8 bits, and
the difference between two reports says which way it turned. Turning it
pages the presets: the page moves forward or back, the preset keeps its
place in the page (so it moves by sixteen), and the page never wraps —
the first page stays the first and the last page stays the last.

**Known gap:** the F1 reports its wheel's position to a PC's HID stack
(verified with hidapi: byte 5 moves ±1 per detent) but **not** to the
P4's — the P4 receives the same 22-byte reports at the same ~100 Hz, with
the same button and knob bytes, but byte 5 stays at zero. Things that
were ruled out: the report rate, the LED report going out (the same
happens with the painting switched off), the class requests a HID host
normally sends (the F1 does not answer SET_IDLE or SET_PROTOCOL — they
never complete, which also blocks every later control transfer), claiming
the device's second interface, and tapping the controller's blue
BROWSE/MIDI button (the press does register in the HID report). The
encoder's *button* works, and shows the page.

## Files

- `usermod_v3_hidctl.h` — the usermod: profile loading, decoding, actions.
- `hid_usb_host.cpp` / `.h` — the USB host client (claims the HID interface).
- `hidctl_types.h` — the profile structures and limits.
- `hidctl_default_profile.h` — the F1 profile that ships with the firmware.

The core is touched in three places: `wled.cpp` (includes the client, calls
`hid_usb_init()` / `hid_usb_poll()` from `usb_task`, drains the app queue),
`usermods_list.cpp` (registers the usermod) and `const.h` (the usermod ID).

## Notes

- Only one HID device is claimed at a time, and only if a profile wants it —
  a keyboard in the board's USB-A port is left alone.
- The MIDI client (`usermod_v3_midi`) explicitly releases the Kontrol F1, so
  the two clients do not race for the device.
- Profiles are read at boot. Restart the board after editing one.

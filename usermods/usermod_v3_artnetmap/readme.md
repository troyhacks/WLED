# Art-Net Output Map usermod

Provides configuration for Art-Net output mapping when WLED is acting as a
**sender** to one or more Art-Net LED controllers (e.g. Advatek, E1.31
bridges, pixel-mapping software). Each "output" is a row in the UI that
maps a range of WLED segments to a span of Art-Net universes, starting at
a configurable universe number. Quick-setup wizard generates many outputs
at once; presets save/load named configurations to LittleFS.

The actual Art-Net *receiver* path is a separate usermod
(`wled00/ArtNetReceiver.cpp`); this one only configures the *sender*.

## What it provides

- Web UI at `/artnetmap` (also linked from the usermod settings page).
- Per-output start universe + LED count.
- Quick-setup (X outputs × Y universes × Z LEDs).
- Save/load named presets to LittleFS as JSON files (`/artnetmap_<name>.json`).
- Test mode that sends 5 s of full-white DMX packets to the configured
  target IP for visual cable/port identification.
- Getters (`getStartUniverse`, `getLedsPerOutput`, `getTargetIP`,
  `getChannelsPerUniverse`) consumed by `wled00/udp.cpp:1151` every
  realtime frame, so changes take effect immediately.

## Data model

For each output `i`:

| Field | Meaning |
|-------|---------|
| `startUniverse[i]` | First Art-Net universe for output `i` (0–32767) |
| `ledsPerOutput[i]` | Total LEDs on this output |
| span | `ceil(leds × bpp / channelsPerUniverse)` universes |

`channelsPerUniverse` is the global setting (510 for RGB, 512 for RGBW /
Advatek). `bpp` is derived from it: 510 → 3, 512 → 4. The web UI offers
the same two options.

The end universe of output `i` is `startUniverse[i] + ceil(leds × bpp /
channelsPerUniverse) - 1`.

## Installation

* `build_flags` = `-D USERMOD_ARTNETMAP`

The usermod is registered automatically in `wled00/usermods_list.cpp:226,
388` once the flag is set; no other build wiring is needed.

## RAM cost

`ARTNETMAP_MAX_OUTPUTS` defaults to **1024** (≈ 6 KB of static state for
`startUniverse[1024]` + `ledsPerOutput[1024]`). Most installations use a
handful of outputs; if RAM is tight (notably on ESP8266), override at
compile time:

```ini
build_flags = -D USERMOD_ARTNETMAP -D ARTNETMAP_MAX_OUTPUTS=64
```

This drops the static cost to ≈ 384 bytes.

## Persistence model

Only two values are written to `cfg.json`:

```json
"ArtNetMap": {
  "enabled": true,
  "currentPreset": "main_rig"
}
```

All other settings (`targetIP`, `channelsPerUniverse`, `padMode`, plus the
per-output arrays) are bundled into the preset JSON files. On boot,
`readFromConfig` reads `currentPreset` and calls `loadPreset()` to restore
the full state. **Changes made through the web UI without saving as a
preset are lost on reboot** — this is intentional, since those values
belong to a named preset, not to the device. Always hit "Save" before
rebooting if you want changes to stick.

## API

All endpoints take `a=<action>` and optional params. They return JSON:
`{"ok":true}` on success, `{"ok":false,"err":"…"}` on failure.

| Action | Params | Effect |
|--------|--------|--------|
| `gen` | `c` (count), `u` (universes/output), `l` (LEDs/output) | Replace all outputs with `count` sequential entries. Validates against Art-Net 15-bit universe limit. |
| `upd` | `i` (index), `u` (universe, optional), `l` (LEDs, optional) | Update one output row. |
| `test` | `i` (index) | Start 5 s test burst on output `i`. |
| `stop` | — | Stop any active test burst. |
| `save` | `n` (name), `ip`, `ch`, `pad` | Save current config as preset `n`. Validates name. |
| `load` | `n` (name) | Load preset `n`. Validates name. |
| `delete` | `n` (name) | Delete preset file. |
| `apply` | `ip`, `ch`, `pad` | Apply in-memory changes (no save) and persist `currentPreset` to cfg.json. |
| `get` | — | Return full current state. |

### Preset name rules

Names must be 1–31 characters and consist only of `[A-Za-z0-9_.-]`.
Names longer than 31 characters, or containing other characters, are
rejected with a clear `err` rather than silently truncating to a
filename collision.

### Universe wrap-around

`gen` refuses parameters that would push the end universe past 32767
(Art-Net's 15-bit universe limit). This prevents silently overlapping
universes when, e.g., `count=1024, universesPerOutput=70`.

## Known issues / not addressed

- **No CSRF / auth on `/artnetmap-api`.** This is a WLED-wide property,
  not specific to this usermod. If you expose WLED beyond a trusted LAN,
  set up firewall rules accordingly.
- **Test mode is one-shot per call.** Tapping Test sends 5 s of full-white
  DMX; the burst stops automatically. Use `stop` to abort early.
- **Per-output bpp is not configurable.** All outputs share the same
  `channelsPerUniverse` setting (and therefore the same bpp). Mixing RGB
  and RGBW controllers on the same WLED instance requires two WLED
  installs.

## Fixes and changes log

### 2026-08 — audit fixes

**Correctness:**

- **H1** Removed dead-code `#ifndef USERMOD_ID_ARTNETMAP #define …
  4200` fallback. Real value (`95`) is defined in `wled00/const.h:151`
  and reaches the usermod via `#include "wled.h"`.
- **H2** `testSeq` in test mode now skips `0` on wrap so the Art-Net
  spec's "sequence 0 = disabled" sentinel never leaks into a receiver
  (matches the same fix at `wled00/udp.cpp:1184-1185`).
- **H3** Test mode now uses the same `bpp` (3 or 4) as the core
  `udp.cpp` output, so RGBW test packets match what the real sender
  emits.
- **H4** `test` and `upd` API actions now bounds-check the index against
  `numOutputs` and return `{"ok":false,"err":"bad idx"}` instead of
  accepting any int and casting into a 16-bit wrap-around.
- **H6** `loadPreset` manual array parser now aborts on no-progress
  input, so a hand-edited preset file with whitespace or non-digit
  characters between `[` and the first number can no longer hang the
  device.

**Memory / conventions:**

- **M1** Default `ARTNETMAP_MAX_OUTPUTS` remains 1024 for backwards
  compatibility (overridable at compile time). The RAM cost is now
  documented at the top of the header and in this README.
- **M2** `servePage` and `handleApi` now re-check `enabled` at request
  time, returning 403 if the usermod has been disabled since boot. No
  need to register/unregister handlers on every toggle.
- **M3** `sendTestPacket` now uses `ARTNET_DEFAULT_PORT` (the
  ESPAsyncE131 macro), matching `wled00/udp.cpp` instead of the
  separate `ARTNET_PORT` from `wled00/ArtNetReceiver.h`.
- **M4** Constructor no longer has a zero-fill loop; the two output
  arrays are value-initialized at member declaration (`= {}`).
- **M5** This README.

**Style:**

- **L1** (reverted) Implementations stay inline in the header. WLED
  usermods are header-only — moving definitions to a `.cpp` breaks the
  build under `-fdata-sections -Wl,--gc-sections` because the static
  `const char[]` members and helpers (referenced only via `FPSTR()` and
  inline lambdas) get stripped from every TU. Keeping them inline in the
  header guarantees every includer emits a definition, which the linker
  merges.
- **L2** `addToJsonInfo` no longer creates a heap `String` for the
  "Art-Net Map" label; passes the `__FlashStringHelper*` directly.
- **L3** `addToConfig` / `readFromConfig` only persist `enabled` and
  `currentPreset` — this is now explicitly documented above and the
  rationale called out in the header.
- **L4** `connected()` remains a no-op stub; test socket is lazy and
  web handlers register on first loop.

**Security / input validation:**

- **S1** Preset names are now validated: 1–31 chars, `[A-Za-z0-9_.-]`
  only. The API returns a clear `err` on rejection instead of silently
  truncating the filename.
- **S2** `gen` now validates that the last output's end universe fits
  in Art-Net's 15-bit range, preventing wrap-around to overlapping
  universes when callers pass large `count × universesPerOutput`
  products. `leds` is capped at 65535 per output.
- **S3** CSRF/auth out of scope (WLED-wide issue).

### Other notes

- Added a `delete` API action (was previously declared in the class but
  only reachable via the FS helper).
- `handleApi` now reports failure reasons on the page (the page's JS
  shows `alert(d.err||'…')` instead of silently reloading).
- `test` action resets `lastTestSend = 0` so the first packet fires
  immediately rather than waiting up to 25 ms.
- `calcMax()` in the page JS now uses the right divisor for both
  channelsPerUniverse values (3 for 510, 4 for 512).

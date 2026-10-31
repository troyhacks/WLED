# AudioReactive usermod

> **Experimental — subject to change.**
> This usermod is under active rework. The measurement path was replaced
> wholesale in 2026-10: the FFT and the transform are gone, the bands are
> measured by filter banks, the AGC is gone, and the display is drawn in one
> window shared by both views. Settings, JSON keys and the serial keys can and
> do change without notice. Nothing here is API-stable yet.

Enables controlling LEDs via audio input. The audio source can be a microphone
or an analog line-in (AUX) through an appropriate adapter. Supported sources
range from analog (MAX4466, MAX9814, …) to digital (INMP441, ICS-43434, …)
and to the `esp_codec_dev` codecs (ES8311, ES8388, ES8374, ES8389, ES7210,
ES7243, ES7243E, ZL38063, CJC8910).

The usermod does the audio processing and publishes a spectrum that specially
written effects can use.

**It does not** provide effects or draw anything to an LED strip or matrix.

---

## How the measurement works

There is **no FFT** in the current build. The transform, its estimator, its
A/B comparison and its fallback are all gone. The bands are measured directly.

### The 31 bands

The measurement scale is the **ISO 266 third-octave set**, 19.95 Hz to
19 952.6 Hz. Each band is the **shaped power** — the power through a bandpass
filter whose ENBW is that band's own width. That is the unit a hardware
analyser shows, which is why these bars can be compared with an RTA bar for
bar.

Two filter banks do the work:

| bank | bands | range | rate |
|---|---|---|---|
| bottom | 8 | 19.95 – 100 Hz | 3 kHz (16× decimated, 400 Hz anti-alias) |
| top | 23 | 125.9 – 19 952.6 Hz | 48 kHz |

Both report in the same unit and on the same scale: white noise steps
+1.00 dB per third-octave band across the boundary, and pink reads flat.

### The 16 display bands

The sixteen bars the WLED effects read **are sixteen of the thirty-one** —
the nearest third-octave centre to each of the old half-octave bands. They
are not a second measurement and not a re-scaling: same audio, same frame,
same unit, same window, same height.

**Which sixteen is the pattern.** A pattern is that selection slid up or down
in whole third-octave bands, so its shape never changes — only where it sits:

| pattern | bottom | top |
|---|---|---|
| Sub Sub - 20Hz | 20 Hz | 5 kHz |
| Sub Bass - 25Hz | 25 Hz | 6.3 kHz |
| Sub Bass - 31.5Hz | 31.5 Hz | 8 kHz |
| Sub Bass - 40Hz | 40 Hz | 10 kHz |
| **Low Bass - 50Hz** *(the default)* | 50 Hz | 12.5 kHz |
| Bass - 63Hz | 63 Hz | 16 kHz |
| Mid Bass - 80Hz | 80 Hz | 20 kHz |

The pattern changes **what a bar means** and nothing else. The measurement is
the thirty-one bands either way, and the thirty-one do not move: this is
which sixteen of them the sixteen bars draw. The calibration and the per-band
trim belong to the RTA band, so they follow the band to wherever it is shown.

The pattern takes effect **immediately** — the bars move to the new bands on
the next frame, with no reboot and nothing recalculated. It is a setting
(`frequency.pattern`), a dropdown on the usermod settings page, and the
serial `z`/`Z` keys.

### The calibration

The input chain (DC blocker, input LPF, and the source's own filtering) is a
filter, and everything downstream measures what came *out* of it. The chain's
response is measured — a unit impulse through the real filter code — and
divided out **once**, at band level, in dB. The correction is clamped to
±12 dB and the number of clamped bands is published.

There is **no target curve**. A "pink" target is the correction for the point
unit, where a pink input reads −3 dB/octave. This measurement is in the
integrated unit, where pink already reads flat; applying the point unit's
correction added a +3 dB/octave slope to the bars. Which reference spectrum
reads flat is a property of the unit, not a setting.

### Level, floor, gate, reference

Four separate quantities, deliberately kept apart:

- **level** — the broadband level of the fresh hop, in dB re full scale.
- **floor** — the noise floor, in the same dB. It snaps *down* instantly and
  creeps *up* with τ = 10 s, and only while the level is still close to it.
  While the signal is present the floor is frozen.
- **gate** — open when the level is `squelch` dB above the floor, shut 4 dB
  below that (hysteresis).
- **reference** — the loudest band's level, **followed**, applied to the
  display only.

Nothing reads its own output, so the deadlocks the old design had are
structurally impossible.

### The display window

The 16 bars and the 31 RTA bars are drawn in **one** window:

```
window = [ reference − span , reference ]
```

The reference **follows** the loudest band, and it follows it two ways:

- **fast** when the signal gets louder (20 ms) — the top of the window jumps
  to the new peak, the bar that caused it is drawn at the **top**, and the
  whole picture is pushed down under it. That visible rescale is the point:
  the bars scale to the full height.
- **slowly** when the signal gets quieter (6 s by default) — the top comes
  back down over seconds rather than with every transient, so the picture
  does not pump.

The release is a setting, and it is tuned live with the serial `w`/`W` keys
(shorter/longer by 500 ms, 100 ms … 30 s), because it decides how long the
display stays pushed down after a peak.

A **latched** maximum — holding the loudest thing ever seen — was tried and
rejected: it cannot overflow and it never creeps, but after the first peak
the bars sat under the top for the rest of the session and never scaled to
100% again.

The reference is dropped after a **real** absence of audio (the gate shut for
3 s), so it is set again once per piece of audio, not once per frame and not
once per gap in the music.

### Timing

The frame is **one hop**: 512 samples at 48 kHz, 10.67 ms, ~93.75 Hz. There
is no window and nothing is carried forward — every measurement already used
only the fresh hop.

The task is **audio-paced, not timer-paced**: the blocking sample read *is*
the pacer, and a frame ends no earlier than one frame period after it
started. A cycle shorter than the frame period drains the DMA ring faster
than the codec fills it; a cycle longer than it grows the ring until the DMA
overwrites samples it has not handed over yet. Both are bad, and the read
holds the cycle at the frame period from below.

---

## Where to read the numbers

- **`/json/ar`** — the machine-readable endpoint. Root key `AudioReactive`.
  Objects: `level`, `peak`, `display`, `geq`, `rta`, `bank`, `rtatopbank`,
  `timing_us` (with `parts`, `analysis_breakdown` and `health`), `calibration`,
  `codec`, `memory`, `self_test`, `udp_sync`. Scalars: `enabled`, `source`,
  `processing`, `input_level`, `input_gain_pct`, `peak_pct`, `init_ms`.
  `geq` carries `pattern`, `pattern_name` and `pattern_shift` — which sixteen
  of the thirty-one the sixteen bars show — and `rta_band`, which says which
  RTA band each of the sixteen is.
- **`/json/info`** — the brief, human-facing rows. Failures only.

`tools/verify_ar.py` checks the whole table and exits 0 only if every row
passed. It reads `http://192.168.6.249/json/ar` by default, or takes a saved
JSON file or another URL.

The rows that matter most:

| what | good value |
|---|---|
| `geq.bars[k]` == `rta.bars[geq.rta_band[k]]` | equal for all 16 — same window, same auto-range |
| `geq.raw_db[k]` == `rta.band_power_db[geq.rta_band[k]]` | identical to 0.000000 dB |
| `memory.frame_samples` / `frame_buf_bytes` | 512 / 4096 — the frame is one hop |
| `timing_us.cycle` | 10 666.7 µs ± 200 |
| `timing_us.parts.deadline_wait` | ~0 — the deadline wait must never fire |
| `timing_us.frames_overran` | 0 |
| `timing_us.health.budget` / `.pacing` / `.dma_surplus` | all `ok` |
| `timing_us.health.read_blocking` | `true` — if false, the DMA ring is filling |
| `bank.ready` / `rtatopbank.ready` / `.healthy` | all `true` |

Do not over-poll `/json/ar`: the `async_tcp` task wedges after roughly 60–70
cumulative GETs, and the board then stops answering HTTP while still
answering ping. A flash resets it.

---

## The serial console

Single characters, because the display is tuned while it is watched: every
key changes one number and prints what it is now.

| key | what it does |
|---|---|
| `t` | run the input-chain self-test |
| `g` | plot the 31-band RTA |
| `G` | plot the 16 display bands |
| `k` / `K` | the filter bank's detector time, shorter / longer by 5 ms |
| `b` / `B` | the window's floor, down / up by 1 dB |
| `u` / `U` | the window's top, down / up by 1 dB |
| `s` / `S` | the auto window's span, narrower / wider by 1 dB |
| `w` / `W` | the auto-range's release, shorter / longer by 500 ms |
| `o` / `O` | the RTA reference offset, down / up by 1 dB |
| `n` / `p` | the band the trim applies to, next / previous |
| `y` / `Y` | that band's trim, down / up by 1 dB |
| `d` / `D` | the dB trim, down / up |
| `c` / `C` | the codec's input gain, down / up by 0.5 dB |
| `z` / `Z` | the 16-band pattern, down / up by one third-octave band |
| `A` | back to the auto-range, and the trim back to its boot value |
| `X` | force reconnect |

The window is **one** window, so touching the floor or the top switches both
views to the **fixed** window — the point of setting a floor is to have a
floor. `s`/`S` is the one control that does not: the span belongs to the auto
window. The reference offset and the per-band trim are calibration, not
display settings, so they survive `A`.

---

## Settings

All runtime, under the `AudioReactive` object in the config JSON.

| key | meaning |
|---|---|
| `analogmic` / `digitalmic` | the source: type, pins, bits, right-slot, master |
| `config.squelch` | the gate's margin above the noise floor, dB |
| `frequency.rtaAuto` | 1 = auto-range, 0 = fixed window |
| `frequency.rtaRange` | the auto window's span, dB (the analyser's "Scale Range") |
| `frequency.rtaFloor` | the fixed window's floor, dB |
| `frequency.rtaTop` | the fixed window's top, dB |
| `frequency.rtaRef` | the reference offset, dB — one number for the whole installation |
| `frequency.rtaTau` | the filter bank's detector time constant, ms |
| `frequency.pattern` | which sixteen of the thirty-one the sixteen bars show, 0…6 |
| `dynamics.limiter` / `rise` / `fall` | the dynamics limiter on the published bars |
| `sync.port` / `mode` / `check_sequence` | UDP sound sync |

### Removed settings

These are ignored, and are removed from a stored config when it is read, so
the settings page does not keep rendering them:

| container | removed |
|---|---|
| `config` | `gain`, `AGC` |
| `experiments` | the whole container — `micLev` (deprecated — the floor tracker subsumes it), `Mic_Quality`, `freqDist`, `I2S_FastPath`, `transform`, and the five band-mutating toggles `AutoLevel`, `BandCompress`, `NoiseFloorSub`, `HistogramNorm`, `Band0Lifter` |
| `frequency` | `scale`, `profile`, `dBtrim`, `dBref`, `dBspan`, `trim`, `rtaTrim`, `target` |

Why they went:

- `scale` and `profile` chose the bar mapping and a per-band gain table.
  Both re-mapped the measured value; a re-mapped band value is not a
  measurement.
- The five experiment toggles each mutated the band values — a per-band
  gain, a per-band compression, a floor subtraction, a histogram
  normalisation, a band-0 lift. Same reason.
- `dBtrim` and `dBspan` were stored settings for the trim and the span. The
  trim is still a live runtime value (the `d`/`D` keys) and starts at 0 at
  every boot; the span is `rtaRange` now.
- `rtaTrim` and `trim` were the per-band trim arrays. The per-band trim is
  a live runtime value (`n`/`p`/`y`/`Y`), not a stored setting.
- `target` was the target curve; see above.
- `transform` — the transform is gone from the usermod.
- `gain` and `AGC` — the per-band gain and the AGC are gone.

The `experiments` container is removed as a whole, not key by key: an empty
object still draws a group heading on the settings page, so the heading goes
with it.

`freqRMS` and `FFT_Window` were already gone before this build and are not
read; they are not in the removal list above, so a config that still carries
them keeps them.

---

## Installation

`build_flags` = `-D USERMOD_AUDIOREACTIVE`

All parameters are runtime configurable. Some need a hard reset after
changing them (I2S microphone or selected GPIOs).

### Compile-time options

| flag | meaning |
|---|---|
| `SR_DMTYPE=x` | the audio source type (see below) |
| `AUDIOPIN=x` | GPIO for the analog microphone / AUX-in (36) |
| `I2S_SDPIN=x` | GPIO for the I2S SD pin (32) |
| `I2S_WSPIN=x` | GPIO for the I2S WS pin (15) |
| `I2S_CKPIN=x` | GPIO for the I2S SCK pin (14) |
| `MCLK_PIN=x` | GPIO for the master clock on digital line-in boards (−1) |
| `ES7243_SDAPIN` | GPIO for the ES7243 I2C SDA pin (−1) |
| `ES7243_SCLPIN` | GPIO for the ES7243 I2C SCL pin (−1) |
| `UM_AUDIOREACTIVE_ENABLE` | the usermod defaults to enabled |
| `UM_AUDIOREACTIVE_DYNAMICS_LIMITER_OFF` | the rise/fall limiter defaults off |
| `SR_SQUELCH=x` | the default gate margin, dB (10) |
| `I2S_USE_RIGHT_CHANNEL` | use RIGHT instead of LEFT (not recommended) |
| `I2S_USE_16BIT_SAMPLES` | 16-bit instead of 32-bit buffers — frees RAM, costs quality |
| `I2S_GRAB_ADC1_COMPLETELY` | experimental; conflicts with any `analogRead()` |
| `MIC_LOGGER` | debugging: log the samples to serial USB |
| `SR_DEBUG` | debugging: extra diagnostics on serial USB |

### Source types (`SR_DMTYPE`)

`0` generic analog · `1` generic I2S · `2` ES7243 · `3` SPH0645 ·
`4` generic I2S with MCLK · `5` generic I2S PDM · `6` ES8388 · `7` WM8978 ·
`8` AC101 · `9` ES8311 at **line level** (the PGA at 0 dB) ·
`19` the same ES8311 at **mic level** (the PGA at +30 dB) ·
`10`–`18` the `esp_codec_dev` codecs (ES8311,
ES8388, ES8374, ZL38063, ES8389, ES7210, ES7243, ES7243E, CJC8910) ·
`51` legacy I2S PDM.

The two ES8311 entries are the same codec on the same pins at different
input levels: a microphone's output is about 30 dB below a line-level
source's, so the PGA's gain has to suit what is plugged in. `9` leaves the
PGA at 0 dB, so a line-level source reaches the ADC as it is instead of
being amplified into overload first; `19` makes up the 30 dB a microphone
needs. Choose `9` for a line-level feed, `19` for a microphone.

### The codec's input gain

`digitalmic:codecGain` is the codec's input gain, in dB, **on top of** the
level the source type chose. It is a trim, not an absolute gain: type `9`
(line level) starts at 0 dB of PGA and type `19` (mic level) starts at
+30 dB, and the trim is added to whichever is in force. Zero — the default —
leaves every codec exactly as it was, so a config that has never heard of the
setting behaves as before.

It is applied **live**: the codec is written and its registers are read back,
so changing it does not need a reboot (unlike the source type itself).

Where it goes, per codec:

| codec | register | resolution | trim range |
|---|---|---|---|
| ES8311 (types `9`, `19`) | `REG17` — the ADC's digital volume | 0.5 dB | −96 … +27 dB (the register itself reaches −100 … +27.5) |
| ES8311 via `esp_codec_dev` (type `10`) | the same `REG17`, written through the library's register access | 0.5 dB | the same |
| ES8388 (type `6`) | registers `16`/`17` — the ADC's digital volume, whose code is an **attenuation** | 0.5 dB | −64 … +32 dB in the line-in setup, −96 … 0 dB in the mic one (the register itself is 0 dB … −96 dB) |
| ES8388 via `esp_codec_dev` (type `11`) | the same registers | 0.5 dB | around its own baseline, which is read from the register when the codec opens |

The trim goes into the **digital volume** and not into the PGA, for three
reasons: the digital volume is half a dB per step against the PGA's 3 dB,
it can attenuate as well as boost, and the PGA is what "line level" and
"mic level" mean — so those two source types keep saying exactly what they
say today.

`REG17` is on the scale Espressif's own ES8311 driver uses: 0.5 dB per code,
`0xBF` = 0 dB, `0x00` = −95.5 dB, `0xFF` = +32 dB. This driver has always
written `0xC8` there — +4.5 dB — and the trim is added to that, so a trim of
0 dB leaves the register byte-for-byte as it was.

The ES8388 counts the other way: its ADC volume code is an attenuation, so
its zero is the code the driver leaves there (0x40 = −32 dB in the line-in
setup, 0x00 = 0 dB in the mic setup) and the trim moves down from there —
which is why the mic setup can only be turned down and the line-in setup can
be turned down or up, by 32 dB.

The other codecs — the ES8374, the ES8389, the ES7210, the ES7243, the
WM8978, the AC101 — have no digital input-gain register that this driver
knows, so the trim does nothing for them: the field is greyed out in the
settings page and the API says so instead of pretending.

The sound-pressure estimate follows the gain: the applied trim is taken off
the estimate, so turning the input up does not also inflate the estimated
sound pressure. The number the estimate uses is the **applied** trim, not the
requested one — the codec's register quantises to its own step and clamps at
its own ends, so the two differ when the request is off the end of the
register's range.

For the same reason the knob follows the chip: a trim that is set to a value
the register cannot reach is stored back as the value the register could
reach, so the settings page and the saved config always say what the codec
is actually doing.

The applied trim is published as `codec.gain_applied_db` on `/json/ar` and as
`codec_gain` in the state object; the requested one is `codec.gain_trim_db`.

---

## Supported hardware

Rewritten for **WLED-MM-P4** by @TroyHacks. Some things may be dropped or
different from upstream, and this code assumes you know what you are doing.

The best results are on the **ESP32-P4** (dual core, PSRAM). The classic
ESP32 and the ESP32-S3 also work. ESP32-S2 and ESP32-C3 may compile but are
not tested — the audio path wants more compute than they have. ESP8266 is not
supported.

Analog audio is only possible on the classic ESP32.

The P4 has no double-precision FPU: the code is float throughout, and the
heavier working sets are placed in PSRAM deliberately. The analysis task's stack
is sized on a measurement, not a guess — its high-water mark is published as
`memory.analysis_task_stack_free`.

---

## Known issues

Open, and worth knowing about:

- **The PDM bandpass is in no calibration.** With a PDM microphone
  (`dmType` 5 or 51) the live chain runs the PDM bandpass filter *before* the
  input chain, and no calibration models it — the calibrations measure the DC
  blocker and the input LPF, which is what the `InputChain` holds. So on the
  PDM path the bottom eight bands read 6.1–19.0 dB low and the top
  twenty-three ~4.7 dB low at 126 Hz, rising to 0.18 dB at 2 kHz — a 4.5 dB
  tilt across them. The self-test cannot see it, because the self-test runs
  the same chain the calibration measures.
  It is left alone deliberately: dividing it out would lift the bands the
  filter exists to quiet by up to 19 dB, putting back the noise it was added
  to remove. Whether the bottom bands should read as the microphone hears them
  or as the filtered signal is a choice, not a bug.
- **A positive display trim saturates the auto window.** The auto-range
  holds the loudest band exactly at the window's 0 dB, and the trim is a dB
  offset applied *on top* of that window — so a positive trim pushes the
  loudest band past the top by exactly the trim amount and the bar pegs at
  255. The input-level slider spans −14 dB to +6 dB, so only its lower half
  does anything other than clip. `A` resets the trim to its boot value.
- The bottom bank's ENBW drifts 0.84983 → 0.84326 across 19.95 – 100 Hz
  against the top bank's flat 0.85. Worth 0.033 dB at the 7/8 boundary;
  it feeds the density/peak path, not the bar heights.
- The bottom bank has no white-noise self-test of its own — only a centred
  tone and a neighbour-leakage check.

### Fixed

- **The level read 0.87 dB low at full scale.** The RMS discarded samples at
  the rail and then divided by the number of *survivors* — so the divisor
  changed with the signal, and the samples it dropped were exactly the
  loudest ones. A 0 dBFS sine lost 16 % of its samples and read 0.87 dB low;
  it only bit above about −0.15 dBFS, which is why it went unnoticed.
  Samples at the rail are now clamped instead, which keeps the divisor
  honest and leaves a full-scale tone reading 0.06 dB low — the honest
  number, since a signal clipped by the ADC really does have less RMS.
- **The calibrations could describe a filter that was not running.** The
  16- and 31-band calibration and the self-test used one set of chain flags
  and the bottom bank used another, and they disagreed with the live path on
  the PDM path (the calibration modelled a DC blocker the PDM path does not
  run) and on `useInputFilter` 0 and 99 (the bottom bank modelled an input
  LPF that is not in the path at all). There is now ONE definition of which
  filters the live chain runs, and the live task, all three calibrations and
  the self-test use it.
- The ESP32-P4 I2S DIN not reading external mic data. The IDF v5 driver's
  `i2s_channel_init_std_mode()` does not disable the GPIO output driver on
  the DIN pin, so the GPIO output register masked the mic signal.
  `gpio_set_direction(din, GPIO_MODE_INPUT)` is now called after the call in
  both `I2SSource::initialize()` and `CodecDevSource::_allocI2sRxChannel()`,
  gated to `CONFIG_IDF_TARGET_ESP32P4`. **Note:** on the Waveshare
  ESP32-P4-Nano, GPIO 45 drives the SD-card power-enable circuit
  ([micropython#19439](https://github.com/micropython/micropython/issues/19439));
  using it for I2S DIN stops the SD card mounting.
- The `esp_codec_dev` sources now honour the UI's `bitsPerSample`,
  `useRightSlot` and `i2sMaster` — they used to be dropped, so those three
  controls silently did nothing on every codec path.
- The old `audioSource` was leaked on every source switch, together with its
  I2S channel. It is now deinitialised and deleted before the switch.
- The `sqrt` macro hack leaked out of this usermod and into 20-odd other
  usermod headers, truncating their double math.

---

## Additional documentation

This usermod is an evolution of [SR-WLED](https://github.com/atuline/WLED),
and a lot of documentation and information can be found in the
[SR-WLED wiki](https://github.com/atuline/WLED/wiki):

* [getting started with audio](https://github.com/atuline/WLED/wiki/First-Time-Setup#sound)
* [Sound settings](https://github.com/atuline/WLED/wiki/Sound-Settings)
* [Digital Audio](https://github.com/atuline/WLED/wiki/Digital-Microphone-Hookup)
* [Analog Audio](https://github.com/atuline/WLED/wiki/Analog-Audio-Input-Options)
* [UDP Sound sync](https://github.com/atuline/WLED/wiki/UDP-Sound-Sync)

---

## Release notes

* 2026-10 The measurement path was replaced: no transform, no target curve,
  no AGC. The bands are measured by two filter banks, the 16 display bands
  are 16 of the 31 RTA bands, and the display is drawn in one window whose
  top follows the loudest band. See "How the measurement works" above.
* 2026-08 Fixed the ESP32-P4 I2S DIN not reading external mic data.
* 2026-06 Moved to pure Espressif IDF v5.5 calls and ESP-DSP FFT
  acceleration, added the `esp_codec_dev` codecs and the AutoLevel
  experiments — by @TroyHacks.
* 2022-11 Aligned with the
  "[MoonModules/WLED](https://amg.wled.me)" audioreactive usermod — by
  @softhack007 (AKA Frank M&ouml;hle).
* 2022-06 Ported from
  "[soundreactive WLED](https://github.com/atuline/WLED)" — by @blazoncek
  (AKA Blaz Kristan) and the
  [SR-WLED team](https://github.com/atuline/WLED/wiki#sound-reactive-wled-fork-team).

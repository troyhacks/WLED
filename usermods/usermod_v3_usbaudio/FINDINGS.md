# USB audio as an AudioReactive source — findings

**Status: work in progress, stopped on 2026-11-23.** The client is written, compiled,
flashed and partly verified on hardware. The audio path itself has never run, because
the board's USB stack refuses to enumerate the device. The blocker is in the IDF build,
not in this code.

Everything here was measured, not assumed. Where a number came from a source file, the
file and line are given.

---

## 1. The question

Can a USB audio device (a microphone, or the capture side of an audio interface) be the
source for the AudioReactive analysis on the ESP32-P4, instead of the board's on-board
ES8311 codec?

The answer has two parts, and they are different in kind.

---

## 2. Feasibility: yes, the isochronous machinery is all there

The ESP-IDF 5.5.1 USB Host stack in this build already contains a complete isochronous
path. Nothing has to be ported. The evidence is in the copies of the driver sources this
repo keeps under `build_output/` (`usbh.c`, `hcd_dwc_v55.c`, `usb_dwc_hal.c`).

**Multi-packet isochronous URBs.** `usb_host_transfer_alloc(size, num_isoc_packets,
&t)` takes a packet count, so one URB can carry many isochronous packets.

**A schedule designed for streaming.** `hcd_dwc_v55.c:2167` `_buffer_fill_isoc()` builds
a **circular** list of 64 qTD descriptors and keeps `next_start_idx`, so the schedule
runs on from one URB into the next. That is why **two URBs have to be in flight**: with
only one, the pipe sits idle between the end of a URB and the submission of its
successor, and the device's packets in that gap are lost.

**The rules an endpoint has to satisfy**, from `usbh.c:275` `transfer_check_usb_compliance()`:

- An isochronous **IN** transfer must request a whole number of MPS per packet. So each
  packet is requested at the endpoint's `wMaxPacketSize`, the device sends less, and the
  real length comes back in `isoc_packet_desc[].actual_num_bytes`.
- Packet *i*'s data sits at `data_buffer + i * MPS` — that is how `_buffer_fill_isoc()`
  lays the qTDs out.

**The schedule maths.** `hcd_dwc_v55.c:1718` computes the pipe's interval as
`1 << (bInterval - 1)` microframes, and `hcd_dwc_v55.c:2582` refuses a URB whose
`num_isoc_packets * interval` exceeds the 64-entry descriptor list. A high-speed
endpoint with `bInterval=4` (one millisecond) therefore admits at most **8 packets per
URB — 8 ms of audio**.

**A free quality metric.** `hcd_dwc_v55.c:2436` `_buffer_parse_isoc()` fills in the
status of **every packet individually**, including `USB_TRANSFER_STATUS_SKIPPED` — "the
packet was skipped due to system latency or bus overload". Every skipped packet is
audio that was lost on the way in, and the driver counts them for us.

### One endpoint kind this driver cannot drive

The driver programs **one transaction per service period**. It does not implement the
"additional transactions" field of a high-bandwidth isochronous endpoint
(`wMaxPacketSize` bits 11-12, `USB_EP_DESC_GET_MULT`). Such an endpoint cannot be
driven correctly, so the client **refuses** it rather than mis-driving it. A plain
48 kHz/16-bit capture endpoint is not high-bandwidth, so this is normally a non-issue.

### The two hard resource limits

| Limit | Where | Consequence |
|---|---|---|
| The DWC receive FIFO is 896 lines and every IN pipe shares it | `usb_dwc_hal.c:238`: `in_mps = (rx_fifo_lines - 2) * 4` | The split already in `wled.cpp` (`nptx 256 / ptx 320 / rx 320`) gives **1272 bytes** of receive FIFO. Audio ISO (1024) + F1 interrupt IN (64) + MIDI interrupt IN (64) = **1152 bytes** — fits as it stands. Add the deck's 512 B interrupt IN or mass storage's 512 B bulk IN and it does **not** fit. |
| 64 qTD descriptors per pipe | `hcd_dwc_v55.c:64` | 8 ms of audio per URB, so two URBs in flight. |
| 16 DWC channels | the P4's HCD | Default pipe + hub + audio (3 pipes) + deck (2) + F1 (2) + MIDI (2) ≈ 11. Fits, but it is not unlimited. |

**The coexistence answer:** USB audio coexists with MIDI, HID and the deck **as long as
USB mass storage is not in the picture at the same time**. That is the trade the
"no Mass Storage" instinct was right about, and the FIFO arithmetic is why.

---

## 3. The blocker: this build cannot enumerate the device

With a USB audio device plugged into COM4's USB-A port, the board's log says:

```
E (17163) ENUM: Configuration descriptor larger than control transfer max length
E (17164) ENUM: [1:3] CHECK_SHORT_CONFIG_DESC FAILED
E (17164) EXT_PORT: [1:3] Port disabled, reset attempts=0
```

The device reaches the bus, is addressed, and is then **refused**.

The limit is `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=256`, in the shipped sdkconfig
(`framework-arduinoespressif32-libs/esp32p4/sdkconfig:2882`). It is a compile-time
constant inside the IDF's `usb` component, used in three places:

- `components/usb/enum.c:553` — the check that refuses the device;
- `components/usb/enum.c:1148` — the enumeration buffer, `urb_alloc(8 + 256)`;
- `components/usb/usb_host.c:1166` — the client's own control transfer, also 256.

The `usb` component is a **prebuilt archive** (`libusb.a`, 2 MB) in the
`arduino-esp32-libs` package, so the value is fixed when that package is built. There is
no runtime knob, and the project cannot change it.

**A USB audio device's configuration descriptor is routinely 300–500 bytes**: an
AudioControl interface with a clock source, an AudioStreaming interface with a format
type, and two endpoints. This is not an exotic device — it is what USB audio is. A
class-compliant UAC1 microphone is often small enough to fit under 256 bytes, which is
why this was not obvious from the start.

The same limit applies to **any** device with a fat descriptor — a camera, a
multi-interface composite device, some HID devices. It is a property of this build, not
of the audio client.

### The same ceiling appears in the HID client, and it is a separate one

The HID control-surface client has two of its own, which raising the IDF's limit would
**not** fix by itself:

- `hid_usb_host.cpp` keeps the device's configuration descriptor in its own
  **256-byte** buffer (`config_desc[256]`, filled at line 2197 and guarded, so a bigger
  descriptor would be silently truncated rather than overflow).
- Its descriptor requests go out with **wIndex = 0** — interface 0 only — and the
  report descriptor is asked for with a 256-byte request.

Both are client-side choices rather than the library's ceiling, so a fix that raises
`CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE` has to raise these as well. The interface
index matters for composite devices: a mouse on a second interface is not seen by the
pointer code for the same reason the audio device is not seen at all.

### The three ways to raise it

| | What it costs | What it risks |
|---|---|---|
| **Rebuild the `arduino-esp32-libs` package** with `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=1024` | The package is built by `lib-builder` from the IDF commit `129cd0d247`; one sdkconfig line changes. But it means building the IDF for the P4 — hours — and it has to be redone whenever the framework moves. | The cleanest result: the whole USB stack is built with the right limit and the WLED tree stays clean. |
| **Vendor the IDF's enumeration module** with the limit raised | `enum.c` plus the four private headers it includes (`usb_private.h`, `usbh.h`, `enum.h`, `hcd.h`), fetched from the exact commit `129cd0d247` so they match the prebuilt archive. Change **one line**: `#define ENUM_CTRL_TRANSFER_MAX_DATA_LEN CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE` → `1024`. The project's objects link before the archives, so the archive's copy of that file is never pulled in, no duplicate state, and the rest of the USB stack is untouched. | IDF internals in the WLED tree, to be re-vendored on every framework update. |
| **Patch the two constants inside the prebuilt `libusb.a`** | The fastest way to prove the isochronous path works today. | A binary patch on a shared package: invisible in review, lost when the package is reinstalled, and it silently affects every other build that uses it. Not recommended. |

**Still unknown: the device's configuration descriptor length.** 512 covers most USB
audio devices; 1024 is safe for anything. It is not expensive to measure — USBTreeView
on a PC shows the Configuration Descriptor's Total Length.

---

## 4. What has been verified on hardware

The client was built, flashed and run against real devices.

**It enumerates and inspects everything.** With the CN003 deck attached:

```
[UAC  19127] device 4: VID=0x1500 PID=0x3002 bcdUSB=2.00 class 0x00 speed HIGH
[UAC  19128] --- configuration descriptor: 66 bytes, 2 interfaces ---
[UAC  19128]   interface 0 alt 0 class 0x03 subclass 0x00 protocol 0x00 eps 2
[UAC  19128]     ep 0x82 INTR mps 512 raw 0x0200 interval 1 mult 0
[UAC  19128]     ep 0x03 INTR mps 1024 raw 0x0400 interval 1 mult 0
[UAC  19128]   interface 1 alt 0 class 0x03 subclass 0x01 protocol 0x01 eps 1
[UAC  19128]     ep 0x81 INTR mps 8 raw 0x0008 interval 10 mult 0
[UAC  19129] device 4: no AudioStreaming interface - not an audio device, releasing
```

It claimed nothing, and the MIDI and HID clients then took the deck normally. The
coexistence design works.

**The diagnostics work.** The client prints, every two seconds, the host's *own* list of
connected device addresses (`usb_host_device_addr_list_fill`). That is what turned
"the audio device is not working" into "the host sees only the root hub" and then into
"the host sees address 1 and the enumeration failed with a size error".

**What has NOT run:** the isochronous stream, the ring buffer, the drift correction, the
UAC2 sample-rate request. None of them can, until the device enumerates.

---

## 5. The design, for whoever picks it up

**Where the code is.** `usermods/usermod_v3_usbaudio/`:

| File | Role |
|---|---|
| `usbaudio_usb_host.h` | The seam to the AudioReactive usermod: six `uac_usb_*` functions. |
| `usbaudio_usb_host.cpp` | The USB Host client. **Not a translation unit** — it is `#include`d into `wled.cpp`, exactly like `midi_usb_host.cpp` and `deck_usb_host.cpp`. Every file-scope symbol needs a `uac_` prefix. |
| `usbaudio_source.h` | `USBHostAudioSource`, the `AudioSource` subclass the analysis selects. |
| `README.md` | How it is wired and what to read when it misbehaves. |

**Why the seam needs external linkage.** The AR usermod is compiled into
`usermods_list.cpp`'s translation unit and the client is `#include`d into `wled.cpp`'s —
different TUs. The samples also cannot go through the app queue: it is five deep and
the F1 reports at 100 Hz.

**Selecting the source.** `dmType` **20** in the AudioReactive config
(`{"AudioReactive":{"digitalmic":{"type":20}}}`), like every other source; changing it
needs a reboot. `arCodecName(20)` is "USB audio". `/json/ar` publishes a `usb` object
(connected, rate, channels, bits, skipped_packets, overruns, underruns, urbs) and a
`codec_name` key — the existing `codec` key is taken over by a nested object further
down, and the second write to a key wins in ArduinoJson.

**Channels.** The analysis is mono and takes the **left** channel, the first sample of
each interleaved frame — the same channel the I2S path reads. One or two channels are
accepted; more is **refused** and logged, not half-understood.

**The endpoint rules.** 48 000 Hz (the analysis' band centres are absolute frequencies,
`SAMPLE_RATE` is a compile-time `constexpr` at `audio_reactive.h:629`, so a 44.1 kHz
stream would put every band on the wrong axis); one or two channels; not high-bandwidth;
MPS no larger than 1024.

**The rate.** A UAC1 device does not put its sample rate in the descriptor — it is
implied by the endpoint's packet size and interval:
`rate = mps * 1000 / (interval * bytes per frame)`. UAC2 has no implied rate at all; the
host sets it with a class-specific SET_CUR (control selector 0x0101, a 32-bit value).
The client also *measures* the rate from the samples that arrive and says so if the two
disagree, because that inference is only as good as the device filling its packets
exactly.

**The ring.** 2048 mono samples in internal RAM (the USB task writes it, the analysis
task reads it, and the transfer buffers cannot be in PSRAM on this build:
`CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM` is off). The two clocks differ — the
device's sample clock and the board's own frame clock, typically 100 ppm apart — so the
reader interpolates and trims its step to hold the ring near its target.

**The teardown.** The transfers are **not freed** when a device goes: they may still be
in flight, and the HCD calls their completion callbacks afterwards. They are detached
(`device_handle = nullptr`) instead, which is what the deck client does for the same
reason. A generation counter tells a stale completion from a live one.

**Still to do:** the UAC2 feedback endpoint (an isochronous OUT pipe carrying the F/X
ratio, for a device whose clock is asynchronous), and the calibration — the board's
+31.3 dB offset is the ES8311's analogue path, and a USB source needs its own for the
sound-pressure figure. The AGC and the display auto-range need nothing.

---

## 6. The tools

- `tools/uac_check.py` — reads `/json/ar` and reports whether the USB source is
  streaming, at the right rate, and whether any audio is being lost.
- `tools/console_boot.py` — captures the console **from the boot** (the port is opened
  with DTR already low, then pulsed, so the reset happens with the port open).

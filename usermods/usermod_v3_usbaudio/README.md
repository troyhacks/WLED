# usermod_v3_usbaudio — a USB audio device as the AudioReactive source

Use a USB audio device (a microphone, or the capture side of an audio
interface) as the source for the AudioReactive analysis, instead of the
board's on-board ES8311 codec.

## Build

```
-D USERMOD_USBAUDIO_USB
```

in the P4 env (`esp32p4_8MB_troyhacks` in `platformio_override.ini`).

## Use

The source is selected the same way every other source is - by the type
number in the AudioReactive config:

```json
{"AudioReactive": {"digitalmic": {"type": 20}}}
```

Type 20 is the USB source. Changing it needs a reboot, like every other
source change.

## Files

| File | Role |
|---|---|
| `usbaudio_usb_host.h` | The seam towards the AudioReactive usermod. Four functions, and nothing else crosses the boundary. |
| `usbaudio_usb_host.cpp` | The USB Host client. **Not a translation unit** - it is `#include`d into `wled.cpp` (see below). |
| `usbaudio_source.h` | `USBHostAudioSource`, the `AudioSource` subclass the analysis selects. |

`usbaudio_usb_host.cpp` is textually included from `wled00/wled.cpp` inside
`#ifdef USERMOD_USBAUDIO_USB`, exactly as `midi_usb_host.cpp` and
`deck_usb_host.cpp` are. Every file-scope symbol in it needs a `uac_`
prefix, and the only non-static symbols are the four in the header above.

Why the seam and not the app queue: the AR usermod is compiled into
`usermods_list.cpp`'s translation unit and the USB client is included into
`wled.cpp`'s. They are different translation units. The queue is also five
deep and the F1 reports at 100 Hz - audio is 48000 samples a second, so the
samples go through a ring buffer instead.

## What it does

The USB Host stack in this build is Espressif's own (`usb/usb_host.h`), and
it already has the whole isochronous path. This client is the missing class
driver:

1. It registers as a USB Host client and claims **only** the audio-class
   interfaces of a device that actually offers an isochronous capture
   endpoint. Any other device is released immediately, so the client never
   starves the MIDI, HID or MSC clients.
2. It walks the configuration descriptor and picks the AudioStreaming
   alternate setting to use.
3. It streams that endpoint with multi-packet isochronous URBs, two in
   flight, and pushes the samples into a ring buffer.
4. `USBHostAudioSource` reads that ring, and the analysis sees a mono source
   exactly like the codec ones.

### The rules the endpoint has to satisfy

| Rule | Why |
|---|---|
| 48 kHz | The analysis' 31 band centres are absolute frequencies. A 44.1 kHz stream would put every band on the wrong axis. |
| One or two channels | The analysis is mono and takes the **left** channel. A surround stream is refused rather than half-understood. |
| Not a high-bandwidth endpoint | This USB Host stack programs one transaction per service period and does not implement the "additional transactions" field of a high-bandwidth endpoint. |
| MPS no larger than 1024 | The USB 2.0 cap for a high-speed isochronous packet, and the size the transfers are allocated for. |

A device that breaks any of them is logged and released, not half-driven.

## Coexistence

The USB audio client is a fourth client on the same High-Speed controller as
the MIDI client, the HID control-surface client and the mass-storage client.
They coexist because each claims its own interfaces, but the controller's
resources are shared:

- **The DWC receive FIFO is 896 lines in total.** Every IN pipe takes from
  the same pool. The audio pipe alone wants 256 lines (a 1024-byte packet),
  the deck's interrupt IN wants 130 and a mass-storage bulk IN another 130.
  Audio + mass storage does not fit; audio + the control surfaces does.
- **The qTD descriptor list holds 64 entries**, and a URB's packet count
  times the endpoint's interval has to fit in it. That caps a URB at eight
  milliseconds of audio, which is why two URBs are kept in flight.
- **The controller has 16 channels**, and each device and each of its pipes
  takes one.

So: USB audio coexists with MIDI and the HID control surfaces. USB mass
storage does not fit alongside it.

## What to look at when it does not work

The client logs everything it sees. The interesting lines are:

- `[UAC] device N: VID=... PID=...` - the device arrived.
- `[UAC] --- configuration descriptor: ...` - the full descriptor, every
  interface, endpoint and class-specific descriptor. **This is the source of
  truth about what the device offers**, and it is the first thing to read.
- `[UAC]   chosen: interface N alt N ep 0xNN mps N interval N -> ...` - the
  alternate setting that was picked, and the format it implies.
- `[UAC] iso: submit of URB N failed: ...` - the controller would not take
  the transfer.
- `[UAC] iso: URB N finished with status N` - the transfer failed. Status 5
  is `SKIPPED`, which means the controller could not service the packet in
  time: that is audio lost.

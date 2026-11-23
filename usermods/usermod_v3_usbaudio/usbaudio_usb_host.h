// usbaudio_usb_host.h
//
// USB Audio (UAC) host client - public interface.
//
// This is the seam between the USB host client (which lives in
// usbaudio_usb_host.cpp and is TEXTUALLY INCLUDED into wled.cpp, the same
// way deck_usb_host.cpp and midi_usb_host.cpp are) and the AudioReactive
// usermod (which is compiled into usermods_list.cpp's translation unit).
//
// Those are two different translation units, so unlike the deck - which hands
// its reports over through the app queue - the sample path needs real
// external linkage. The functions below are the only non-static symbols the
// client defines.
//
// Why not the app queue? The queue is five deep and the F1 reports at 100 Hz;
// audio is 48000 samples a second. Samples go through a ring buffer instead,
// and the queue is not involved.
//
// Build: -D USERMOD_USBAUDIO_USB on the P4 env (esp32p4_8MB_troyhacks).

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef USERMOD_USBAUDIO_USB

// Called once from usb_task, after the host stack is installed.
void uac_usb_init(void);

// Called on every usb_task iteration.
void uac_usb_poll(void);

// True while an audio device is claimed and streaming.
bool uac_usb_connected(void);

// The sample rate the device is streaming at, in Hz. Zero when no device.
uint32_t uac_usb_sample_rate(void);

// The format the device is streaming: channels and bits per sample.
// Zero when no device.
uint32_t uac_usb_channels(void);
uint32_t uac_usb_bits_per_sample(void);

// Read `n` mono samples into `dst` as floats in the range -1.0 .. +1.0.
//
// Blocks until `n` samples are available, the device goes away, or
// `timeout_ms` expires. On a timeout the tail is filled with silence and the
// function returns false - the analysis must never hang on a device that
// stopped talking.
//
// The reader also corrects the clock difference between the device and the
// board's own frame clock, so the rate it delivers is the device's rate even
// though the two crystals differ.
bool uac_usb_read_samples(float* dst, uint16_t n, uint32_t timeout_ms);

// Counters, for the info page and for the bench.
void uac_usb_get_stats(uint32_t* skipped,   // ISO packets the controller skipped
                       uint32_t* overruns,  // samples dropped: the ring was full
                       uint32_t* underruns, // frames that had to be padded with silence
                       uint32_t* urbs);     // ISO transfers completed

#endif // USERMOD_USBAUDIO_USB

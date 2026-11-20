// hid_usb_host.h
//
// Generic USB Host client for HID control surfaces (ESP32-P4).
//
// The client claims the HID interface (USB class 3) of a device that one of
// the loaded profiles (see usermod_v3_hidctl.h) wants, and posts the raw
// input reports to the app queue. It knows nothing about any particular
// controller: what a report means is entirely up to the profile.
//
// The client deliberately does NOT claim every HID device it sees — a
// keyboard plugged into the board's USB-A port would be swallowed. It only
// claims a device when a profile matches its VID/PID, which keeps the
// other USB host clients (MIDI, deck, MSC) out of the way as well.

#pragma once

#include <stdint.h>

#ifdef USERMOD_HIDCTL_USB

#include "hidctl_types.h"

// Called once from usb_task, after the host stack is installed.
void hid_usb_init(void);

// Called on every usb_task iteration.
void hid_usb_poll(void);

// True while a HID device is claimed.
bool hid_usb_connected(void);

#endif  // USERMOD_HIDCTL_USB

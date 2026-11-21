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

// Send an output report (LEDs, display). `data` is the report's data
// ONLY - the report ID goes in the control transfer's wValue, not in the
// data. Returns false when the device is not there or a transfer is already
// in flight.
bool hid_usb_send_output_report(uint8_t report_id, const uint8_t* data, uint16_t len);

// Send data on the interface's OUT endpoint (some controllers take their
// LED data there rather than over the control pipe).
bool hid_usb_send_out_endpoint(const uint8_t* data, uint16_t len);
int8_t  hid_usb_last_out_status(void);

// The bytes a device-to-host control transfer brought back (the LED state
// readback). Returns the buffer and its length.
const uint8_t* hid_usb_readback(uint8_t* len);

// The last report that went out on the OUT endpoint (the LED report).
const uint8_t* hid_usb_last_out_report(uint8_t* len);

// Send a report with an explicit type: 2 = Output, 3 = Feature. Used to
// work out which shape a controller actually wants.
bool hid_usb_send_report(uint8_t report_type, uint8_t report_id,
                         const uint8_t* data, uint16_t len);

// A control transfer with the setup packet spelled out.
bool hid_usb_send_setup(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                        const uint8_t* data, uint16_t len);

// The same, with an explicit interface index (wIndex).
bool hid_usb_send_setup_to(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                           uint8_t wIndex, const uint8_t* data, uint16_t len);

// The status of the last class request (SET_PROTOCOL / SET_IDLE):
// -1 = not sent, 0 = completed, 4 = stalled.
int8_t  hid_usb_class_status(void);

// The configuration descriptor, exactly as the device sent it. This is the
// authority on how many interfaces the device has and how many endpoints
// each one has.
const uint8_t* hid_usb_config_desc(uint8_t* len);

// The reports that have come in, and the zero-length packets that have
// followed them (the F1 sends one after every report).
uint32_t hid_usb_report_count(void);
uint32_t hid_usb_zlp_count(void);

// The outcome of the last output report: 0 = completed, 4 = stalled, -1 =
// none sent yet. And how many reports have gone through.
int8_t  hid_usb_last_report_status(void);
uint32_t hid_usb_reports_ok(void);

// What the device's configuration descriptor says about its interfaces.
const HidctlIfaceInfo* hid_usb_interfaces(uint8_t* count);

// Ask the device for a descriptor. bmRequestType is 0x80 for a device-type
// descriptor (device, configuration) and 0x81 for an interface-type one (the
// HID report descriptor). wValue selects it: 0x0100 device, 0x0200
// configuration, 0x2200 HID report descriptor. The bytes are logged.
bool hid_usb_get_descriptor(uint8_t bmRequestType, uint16_t wValue,
                            uint16_t wLength, uint8_t wIndex);

#endif  // USERMOD_HIDCTL_USB

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

// What KIND of HID device this is.
//
// A control surface is driven by a profile - its buttons and its knobs
// are mapped to WLED's actions.  A mouse and a keyboard are not: they are
// pointed at the screen, and the screen is not a USB device.  They are
// recognised by the HID interface's own subclass and protocol, because a
// boot mouse's report layout is the same for every mouse there is.
#define HID_KIND_SURFACE  0   // a control surface: driven by a profile
#define HID_KIND_POINTER  1   // a mouse: pointed at the screen
#define HID_KIND_KEYBOARD 2   // a keyboard

// Called once from usb_task, after the host stack is installed.
void hid_usb_init(void);

// Called on every usb_task iteration.
void hid_usb_poll(void);

// True while a HID device is claimed.
bool hid_usb_connected(uint8_t d);

// Send an output report (LEDs, display). `data` is the report's data
// ONLY - the report ID goes in the control transfer's wValue, not in the
// data. Returns false when the device is not there or a transfer is already
// in flight.
bool hid_usb_send_output_report(uint8_t d, uint8_t report_id, const uint8_t* data, uint16_t len);

// Send data on the interface's OUT endpoint (some controllers take their
// LED data there rather than over the control pipe).
// Queue a packet for a device's interrupt OUT endpoint. Safe from any
// task: it copies the bytes and returns. The task that owns the USB stack
// sends them, one at a time, when the pipe is free.
bool hid_usb_queue_out(uint8_t d, const uint8_t* data, uint16_t len);
int8_t hid_usb_last_out_status(uint8_t d);

// The bytes a device-to-host control transfer brought back (the LED state
// readback). Returns the buffer and its length.
const uint8_t* hid_usb_readback(uint8_t d, uint8_t* len);

// The last report that went out on the OUT endpoint (the LED report).
const uint8_t* hid_usb_last_out_report(uint8_t d, uint8_t* len);

// Send a report with an explicit type: 2 = Output, 3 = Feature. Used to
// work out which shape a controller actually wants.
bool hid_usb_send_report(uint8_t d, uint8_t report_type, uint8_t report_id,
                         const uint8_t* data, uint16_t len);

// A control transfer with the setup packet spelled out.
bool hid_usb_send_setup(uint8_t d, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                        const uint8_t* data, uint16_t len);

// The same, with an explicit interface index (wIndex).
bool hid_usb_send_setup_to(uint8_t d, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                           uint8_t wIndex, const uint8_t* data, uint16_t len);

// The wire log: every report that differed from the one before, with the
// number of packets that came in between.
struct HidLogEntry {
  uint16_t skipped;
  uint8_t  data[HIDCTL_MAX_REPORT];
  uint8_t  len;
};
// The log in chronological order: index 0 is the oldest entry kept.
const HidLogEntry* hid_usb_log(uint8_t index);
uint8_t hid_usb_log_count(void);
uint32_t hid_usb_log_reports(void);
uint32_t hid_usb_log_zlps(void);

// The status and length of the last sixteen IN completions, and the count
// of completions that came back with a status other than COMPLETED.
const uint8_t* hid_usb_in_statuses(uint8_t d, uint8_t* count, uint16_t** lengths);
uint32_t hid_usb_in_errors(uint8_t d);

// The status of the last class request (SET_PROTOCOL / SET_IDLE):
// -1 = not sent, 0 = completed, 4 = stalled.
int8_t hid_usb_class_status(uint8_t d);

// The answer to the last class request, if it brought one back.
const uint8_t* hid_usb_class_data(uint8_t d, uint8_t* len);

// The stage of the class-request sequence: 0 = not started, 1 = the vendor
// request due, 2 = SET_IDLE due, 3 = done.
uint8_t hid_usb_class_stage(uint8_t d);

// Ask for the class requests again, so they can be tried without a
// rebuild.
void hid_usb_request_class(uint8_t d);

// Turn the class requests on or off. They are off by default: the F1
// stalls them and the stalled control endpoint is never cleared here.
void hid_usb_set_class_requests(uint8_t d, bool want);

// Send the wake-up packet again: {0xA0, 0x00, 0x00} on the OUT
// endpoint.
void hid_usb_send_wakeup(uint8_t d);

// Ask for the feature reports too (or stop asking). A PC's HID stack
// never asks for them.
void hid_usb_set_feature_reports(uint8_t d, bool want);
void hid_usb_set_class_vendor_request(uint8_t d, bool want);
bool hid_usb_get_class_vendor_request(uint8_t d);

// Claim or release the device's OTHER interface (the F1's second one). A
// PC's HID stack leaves it alone.
void hid_usb_set_other_interface(uint8_t d, bool claim);
uint8_t hid_usb_other_iface(uint8_t d);
bool    hid_usb_other_claimed(uint8_t d);

// The F1's feature report, if the device answered the request for it.
const uint8_t* hid_usb_feature_data(uint8_t d, uint8_t* len);

// The configuration descriptor, exactly as the device sent it. This is the
// authority on how many interfaces the device has and how many endpoints
// each one has.
const uint8_t* hid_usb_config_desc(uint8_t d, uint8_t* len);

// The reports that have come in, and the zero-length packets that have
// followed them (the F1 sends one after every report).
uint32_t hid_usb_report_count(uint8_t d);
uint32_t hid_usb_out_count(uint8_t d);
uint32_t hid_usb_out_refused(uint8_t d, uint8_t which);   // 0 no device, 1 too long,
                                                // 2 busy, 3 the host said no
int8_t hid_usb_out_submit_error(uint8_t d);
uint32_t hid_usb_out_stuck(uint8_t d);
bool    hid_usb_out_failed(uint8_t d);
bool    hid_usb_out_broken(uint8_t d);
uint8_t hid_usb_out_recovery(uint8_t d);
uint16_t hid_usb_out_queued(uint8_t d);     // transfers that never finished
uint32_t hid_usb_ctrl_out_count(uint8_t d);   // output reports over the control pipe
uint32_t hid_usb_out_bytes(uint8_t d);    // how many bytes went on the OUT pipe
uint8_t hid_usb_link_speed(uint8_t d);   // 0 unknown, 1 full speed, 2 high speed
void hid_usb_set_out_via_control(uint8_t d, bool on);
bool hid_usb_get_out_via_control(uint8_t d);
void hid_usb_set_out_drop_leading(uint8_t d, bool on);
bool hid_usb_get_out_drop_leading(uint8_t d);
void hid_usb_set_out_shift(uint8_t d, bool on);
bool hid_usb_get_out_shift(uint8_t d);
void hid_usb_set_out_cap(uint8_t d, uint16_t n);
uint16_t hid_usb_get_out_cap(uint8_t d);
void hid_usb_set_out_zlp(uint8_t d, bool on);
bool hid_usb_get_out_zlp(uint8_t d);
void hid_usb_set_in_xfer_len(uint8_t d, uint16_t n);
uint16_t hid_usb_get_in_xfer_len(uint8_t d);
void hid_usb_reset_in_pipe(uint8_t d);
void hid_usb_set_idle_rate(uint8_t d, uint16_t ms);
uint16_t hid_usb_get_idle_rate(uint8_t d);
void hid_usb_set_idle_rate_low_byte(uint8_t d, bool on);
bool hid_usb_get_idle_rate_low_byte(uint8_t d);
bool hid_usb_set_in_pid(uint8_t d, uint8_t pid);
bool hid_usb_read_firmware(uint8_t d);
const uint8_t* hid_usb_out_first(uint8_t d, uint8_t* len);
const uint8_t* hid_usb_report_desc(uint8_t d, uint16_t* len);
// What KIND of device this is: HID_KIND_SURFACE (driven by a profile),
// HID_KIND_POINTER (a mouse, pointed at the screen) or HID_KIND_KEYBOARD.
uint8_t hid_usb_device_kind(uint8_t d);
const uint8_t* hid_usb_last_out_head(uint8_t d, uint8_t* len);
void hid_usb_out_watchdog(uint8_t d);
uint32_t hid_usb_zlp_count(uint8_t d);

// The outcome of the last output report: 0 = completed, 4 = stalled, -1 =
// none sent yet. And how many reports have gone through.
int8_t hid_usb_last_report_status(uint8_t d);
uint32_t hid_usb_reports_ok(uint8_t d);

// What the device's configuration descriptor says about its interfaces.
const HidctlIfaceInfo* hid_usb_interfaces(uint8_t d, uint8_t* count);

// Ask the device for a descriptor. bmRequestType is 0x80 for a device-type
// descriptor (device, configuration) and 0x81 for an interface-type one (the
// HID report descriptor). wValue selects it: 0x0100 device, 0x0200
// configuration, 0x2200 HID report descriptor. The bytes are logged.
bool hid_usb_get_descriptor(uint8_t d, uint8_t bmRequestType, uint16_t wValue,
                            uint16_t wLength, uint8_t wIndex);

#endif  // USERMOD_HIDCTL_USB

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
const uint8_t* hid_usb_in_statuses(uint8_t* count, uint16_t** lengths);
uint32_t hid_usb_in_errors(void);

// The status of the last class request (SET_PROTOCOL / SET_IDLE):
// -1 = not sent, 0 = completed, 4 = stalled.
int8_t  hid_usb_class_status(void);

// The answer to the last class request, if it brought one back.
const uint8_t* hid_usb_class_data(uint8_t* len);

// The stage of the class-request sequence: 0 = not started, 1 = the vendor
// request due, 2 = SET_IDLE due, 3 = done.
uint8_t hid_usb_class_stage(void);

// Ask for the class requests again, so they can be tried without a
// rebuild.
void hid_usb_request_class(void);

// Turn the class requests on or off. They are off by default: the F1
// stalls them and the stalled control endpoint is never cleared here.
void hid_usb_set_class_requests(bool want);

// Send the wake-up packet again: {0xA0, 0x00, 0x00} on the OUT
// endpoint.
void hid_usb_send_wakeup(void);

// Ask for the feature reports too (or stop asking). A PC's HID stack
// never asks for them.
void hid_usb_set_feature_reports(bool want);
void hid_usb_set_class_vendor_request(bool want);
bool hid_usb_get_class_vendor_request(void);

// Claim or release the device's OTHER interface (the F1's second one). A
// PC's HID stack leaves it alone.
void hid_usb_set_other_interface(bool claim);
uint8_t hid_usb_other_iface(void);
bool    hid_usb_other_claimed(void);

// The F1's feature report, if the device answered the request for it.
const uint8_t* hid_usb_feature_data(uint8_t* len);

// The configuration descriptor, exactly as the device sent it. This is the
// authority on how many interfaces the device has and how many endpoints
// each one has.
const uint8_t* hid_usb_config_desc(uint8_t* len);

// The reports that have come in, and the zero-length packets that have
// followed them (the F1 sends one after every report).
uint32_t hid_usb_report_count(void);
uint32_t hid_usb_out_count(void);
uint32_t hid_usb_out_refused(uint8_t which);   // 0 no device, 1 too long,
                                                // 2 busy, 3 the host said no
int8_t  hid_usb_out_submit_error(void);
uint32_t hid_usb_out_stuck(void);     // transfers that never finished
uint32_t hid_usb_ctrl_out_count(void);   // output reports over the control pipe
uint32_t hid_usb_out_bytes(void);    // how many bytes went on the OUT pipe
uint8_t  hid_usb_link_speed(void);   // 0 unknown, 1 full speed, 2 high speed
void     hid_usb_set_out_via_control(bool on);
bool     hid_usb_get_out_via_control(void);
void     hid_usb_set_out_drop_leading(bool on);
bool     hid_usb_get_out_drop_leading(void);
void     hid_usb_set_out_shift(bool on);
bool     hid_usb_get_out_shift(void);
void     hid_usb_set_out_cap(uint16_t n);
uint16_t hid_usb_get_out_cap(void);
void     hid_usb_set_out_zlp(bool on);
bool     hid_usb_get_out_zlp(void);
void     hid_usb_set_in_xfer_len(uint16_t n);
uint16_t hid_usb_get_in_xfer_len(void);
void     hid_usb_reset_in_pipe(void);
void     hid_usb_set_idle_rate(uint16_t ms);
uint16_t hid_usb_get_idle_rate(void);
bool     hid_usb_pump_out(void);   // sends the pending zero-length packet
const uint8_t* hid_usb_out_first(uint8_t* len);
const uint8_t* hid_usb_report_desc(uint16_t* len);
const uint8_t* hid_usb_last_out_head(uint8_t* len);
void     hid_usb_out_watchdog(void);
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

// usb_host_messages.h
// Shared USB Host app_message_t used by wled.cpp (producer/consumer) and
// midi_usb_host.cpp (producer of MIDI events).
//
// Lives in the usermod_v3_midi directory because it started out there, but it
// is now the shared host message type — each control-surface usermod adds its
// own event IDs and union members here rather than defining a parallel type
// that wled.cpp would also have to drain.
//
// Gated by SOC_USB_OTG_SUPPORTED (only the P4 USB Host subsystem uses
// it). The underlying msc_host_device_handle_t / usb_host includes
// are ESP-IDF headers, so we only attempt to include them when the
// target supports it.

#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef SOC_USB_OTG_SUPPORTED

#include "usb/usb_host.h"
#include "usb/msc_host_vfs.h"

#ifdef USERMOD_HIDCTL_USB
// HIDCTL_MAX_REPORT — the size of the raw HID report the message carries.
#include "../usermod_v3_hidctl/hidctl_types.h"
#endif

typedef struct {
  enum {
    APP_QUIT = 0,                  // Signals request to exit the application
    APP_DEVICE_CONNECTED = 1,      // MSC device connect event
    APP_DEVICE_DISCONNECTED = 2,   // MSC device disconnect event
#ifdef USERMOD_MIDI_USB
    APP_MIDI_PACKET = 10,          // Inbound MIDI message from controller (parsed)
    APP_MIDI_DEVICE_CONNECTED = 11,    // Controller enumerated
    APP_MIDI_DEVICE_DISCONNECTED = 12, // Controller gone
#endif
#ifdef USERMOD_HIDCTL_USB
    APP_HIDCTL_PACKET = 30,                // Inbound HID report from a control surface
    APP_HIDCTL_DEVICE_CONNECTED = 31,      // Control surface enumerated
    APP_HIDCTL_DEVICE_DISCONNECTED = 32,   // Control surface gone
#endif
  } id;
  union {
    uint8_t new_dev_address;                  // MSC device address for APP_DEVICE_CONNECTED
    msc_host_device_handle_t device_handle;   // MSC handle for APP_DEVICE_DISCONNECTED
#ifdef USERMOD_MIDI_USB
    struct {
      uint8_t status;        // Channel-stripped command byte (0x80/0x90/0xB0/0xC0/0xE0)
      uint8_t data1;
      uint8_t data2;
    } midi;
    // Device descriptor populated by the USB Host client when posting
    // APP_MIDI_DEVICE_CONNECTED. wled.cpp publishes a UsbDeviceChanged
    // event with this payload.
    struct {
      uint16_t vid;
      uint16_t pid;
      char     name[24];
    } midi_device_info;
#endif
#ifdef USERMOD_HIDCTL_USB
    // The raw HID input report, straight from the device. What it means is
    // decided by the profile (see usermod_v3_hidctl).
    struct {
      uint8_t  device;      // which device on the bus the report came from
      uint16_t len;
      uint8_t  data[HIDCTL_MAX_REPORT];
    } hidctl;
    struct {
      uint8_t  device;      // the device on the bus
      uint16_t vid;
      uint16_t pid;
      char     name[24];
    } hidctl_device_info;
#endif
  } data;
} app_message_t;

#endif // SOC_USB_OTG_SUPPORTED
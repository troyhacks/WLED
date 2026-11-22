// hidctl_types.h
//
// Types and constants for the generic USB HID control surface usermod.
//
// Profiles live on the filesystem in the LittleFS root, one JSON file per
// controller, named "<prefix><name>.json" so the loader can tell a profile
// from cfg.json / presets.json without needing a folder.

#pragma once

#include <stdint.h>

// Profile files live in the LittleFS root (no folder).
#define HIDCTL_PROFILE_DIR        "/"
#define HIDCTL_PROFILE_PREFIX     "hidctl-"
#define HIDCTL_PROFILE_SUFFIX     ".json"
#define HIDCTL_DEFAULT_PROFILE_FILE "hidctl-traktor-kontrol-f1.json"

// Limits. They are deliberately small: a profile is a few hundred bytes of
// JSON, and the tables live in PSRAM.
//
// The report sizes are the exception: they are set by the device, not by
// the profile. The F1's input report is 22 bytes and its LED report 81,
// but the OpenDeck-family decks send 512-byte input reports and take
// 1025-byte output packets, so the buffers have to be big enough for the
// worst of them.
#define HIDCTL_MAX_PROFILES        2
#define HIDCTL_MAX_CONTROLS       16
#define HIDCTL_MAX_ENCODERS        8
#define HIDCTL_MAX_BUTTONS        32
#define HIDCTL_MAX_BUTTON_BYTES    4
#define HIDCTL_MAX_REPORT       1024
#define HIDCTL_MAX_NAME           24
#define HIDCTL_MAX_ACTION         20
#define HIDCTL_MAX_LIGHTS         40
#define HIDCTL_MAX_SEGMENTS        8
#define HIDCTL_MAX_DIGITS          4
#define HIDCTL_MAX_OUTPUT       1025
#define HIDCTL_MAX_IFACES          4
#define HIDCTL_MAX_IFACE_EPS       4
#define HIDCTL_READBACK_BYTES     96

// One continuous control (a knob, a fader, a wheel): a little-endian
// unsigned integer of `bits` bits at `offset` in the report.
struct HidctlControl {
  char     name[HIDCTL_MAX_NAME];
  char     action[HIDCTL_MAX_ACTION];
  char     mod_action[HIDCTL_MAX_ACTION];   // what it drives with a modifier held
  uint16_t offset;
  uint8_t  bits;
  uint8_t  taken_over;
};

// One rotary encoder of an OpenDeck-family deck. It has no absolute
// position: it reports a twist (plus or minus one) and a push, so what it
// drives is a value this usermod keeps.
struct HidctlEncoder {
  char     name[HIDCTL_MAX_NAME];
  char     action[HIDCTL_MAX_ACTION];       // what a twist drives
  char     push_action[HIDCTL_MAX_ACTION];   // what the push does
  uint8_t  value;                           // the last value it drove
};

// One button: a bit of the button bitmap.
struct HidctlButton {
  char     name[HIDCTL_MAX_NAME];
  char     action[HIDCTL_MAX_ACTION];       // plain press
  char     mod_action[HIDCTL_MAX_ACTION];   // press while a modifier is held
  uint8_t  index;                          // global button index
  uint8_t  modifier;                       // true for modifier keys (SHIFT)
};

// One light in the output report (an LED or a group of them).
struct HidctlLight {
  char     name[HIDCTL_MAX_NAME];
  uint16_t offset;      // byte offset in the report (the ID byte is 0)
  uint8_t  bytes;       // 1 = one brightness, 2 = two, 3 = blue, red, green
};

// The seven-segment display in the output report.
struct HidctlDisplay {
  bool     present;
  uint8_t  digit_count;
  uint16_t digit_offset[HIDCTL_MAX_DIGITS];
  uint16_t dot_offset;      // 0xFFFF = no dot
  uint8_t  segments_per_digit;
  // The segment order, as names. The usermod maps a digit to strokes by name.
  char     segment_names[HIDCTL_MAX_SEGMENTS][12];
};

// The output (host to device) report: LEDs and the display.
struct HidctlOutput {
  bool     present;
  uint8_t  report_id;
  uint16_t length;       // bytes in the report, including the ID byte
  HidctlDisplay display;
  uint8_t  light_count;
  HidctlLight lights[HIDCTL_MAX_LIGHTS];
};

// What the device's configuration descriptor says about one interface.
struct HidctlIfaceInfo {
  uint8_t num;        // interface number
  uint8_t cls;        // bInterfaceClass (3 = HID)
  uint8_t sub;        // bInterfaceSubClass
  uint8_t proto;      // bInterfaceProtocol
  uint8_t ep_count;
  uint8_t ep_addr[HIDCTL_MAX_IFACE_EPS];
  uint16_t ep_mps[HIDCTL_MAX_IFACE_EPS];
  uint8_t ep_attr[HIDCTL_MAX_IFACE_EPS];   // bmAttributes (2 = bulk, 3 = interrupt)
};

// One controller.
struct HidctlProfile {
  char     name[HIDCTL_MAX_NAME];
  uint16_t vid;
  uint16_t pid;
  uint8_t  report_id;       // 0 = accept any
  uint16_t report_len;      // bytes, including the report ID byte
  uint8_t  button_bytes[HIDCTL_MAX_BUTTON_BYTES];
  uint8_t  button_byte_count;
  uint8_t  msb_first;       // true = bit 7 of the first byte is button 0
  uint8_t  control_count;
  uint8_t  button_count;
  HidctlControl controls[HIDCTL_MAX_CONTROLS];
  HidctlButton buttons[HIDCTL_MAX_BUTTONS];
  // The rotary encoders of an OpenDeck-family deck.
  uint8_t         encoder_count;
  HidctlEncoder   encoders[HIDCTL_MAX_ENCODERS];
  HidctlOutput output;
  // The wheel: a byte in the report that counts the encoder's position.
  bool     wheel_present;
  uint16_t wheel_offset;
  // An OpenDeck-family deck (the Soomfon CN003 and the Mirabox N4 / Ajazz
  // AKP05 family) is not a plain HID device: its input reports are single
  // events with a message ID in them, and its output is the ASCII "CRT"
  // command protocol rather than an LED report.
  bool     opendeck;
};

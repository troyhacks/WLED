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
#define HIDCTL_MAX_PROFILES        2
#define HIDCTL_MAX_CONTROLS       16
#define HIDCTL_MAX_BUTTONS        32
#define HIDCTL_MAX_BUTTON_BYTES    4
#define HIDCTL_MAX_REPORT         64
#define HIDCTL_MAX_NAME           24
#define HIDCTL_MAX_ACTION         20

// One continuous control (a knob, a fader, a wheel): a little-endian
// unsigned integer of `bits` bits at `offset` in the report.
struct HidctlControl {
  char     name[HIDCTL_MAX_NAME];
  char     action[HIDCTL_MAX_ACTION];
  uint16_t offset;
  uint8_t  bits;
  uint8_t  taken_over;
};

// One button: a bit of the button bitmap.
struct HidctlButton {
  char     name[HIDCTL_MAX_NAME];
  char     action[HIDCTL_MAX_ACTION];       // plain press
  char     mod_action[HIDCTL_MAX_ACTION];   // press while a modifier is held
  uint8_t  index;                          // global button index
  uint8_t  modifier;                       // true for modifier keys (SHIFT)
};

// One controller.
struct HidctlProfile {
  char     name[HIDCTL_MAX_NAME];
  uint16_t vid;
  uint16_t pid;
  uint8_t  report_id;       // 0 = accept any
  uint8_t  report_len;      // bytes, including the report ID byte
  uint8_t  button_bytes[HIDCTL_MAX_BUTTON_BYTES];
  uint8_t  button_byte_count;
  uint8_t  msb_first;       // true = bit 7 of the first byte is button 0
  uint8_t  control_count;
  uint8_t  button_count;
  HidctlControl controls[HIDCTL_MAX_CONTROLS];
  HidctlButton buttons[HIDCTL_MAX_BUTTONS];
};

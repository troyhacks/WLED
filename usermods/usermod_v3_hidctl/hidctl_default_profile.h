// hidctl_default_profile.h
//
// The profile that ships with the firmware: the Traktor Kontrol F1.
//
// It is written to the LittleFS root as /hidctl-traktor-kontrol-f1.json on
// first boot, so it can be read, edited, copied or deleted like any other
// profile file.
//
// The F1 is a USB HID device (class 3) with NI's vendor usage page 0xFF01.
// Its 22-byte input report (report ID 0x01) is:
//
//   byte  0       report ID
//   bytes 1..4    button bitmaps (bit 7 of byte 1 is button 0)
//   byte  5       status byte
//   bytes 6..21   eight 12-bit little-endian values
//
// The four FILTER knobs (centre detent at ~2048) come first, then the four
// channel faders (0 at the bottom).

#pragma once

static const char HIDCTL_DEFAULT_PROFILE_JSON[] = R"JSON({
  "name": "Traktor Kontrol F1",
  "vid": "0x17CC",
  "pid": "0x1120",
  "report_id": 1,
  "report_len": 22,
  "buttons": { "bytes": [1, 2, 3, 4], "msb_first": true },
  "controls": [
    { "name": "knob1",  "offset": 6,  "bits": 12, "action": "bri" },
    { "name": "knob2",  "offset": 8,  "bits": 12, "action": "effectSpeed" },
    { "name": "knob3",  "offset": 10, "bits": 12, "action": "effectIntensity" },
    { "name": "knob4",  "offset": 12, "bits": 12, "action": "effectPalette" },
    { "name": "fader1", "offset": 14, "bits": 12, "action": "seg0bri" },
    { "name": "fader2", "offset": 16, "bits": 12, "action": "seg1bri" },
    { "name": "fader3", "offset": 18, "bits": 12, "action": "seg2bri" },
    { "name": "fader4", "offset": 20, "bits": 12, "action": "seg3bri" }
  ],
  "button_map": [
    { "index": 0,  "name": "pad1",  "action": "preset1",  "mod_action": "savepreset1" },
    { "index": 1,  "name": "pad2",  "action": "preset2",  "mod_action": "savepreset2" },
    { "index": 2,  "name": "pad3",  "action": "preset3",  "mod_action": "savepreset3" },
    { "index": 3,  "name": "pad4",  "action": "preset4",  "mod_action": "savepreset4" },
    { "index": 4,  "name": "pad5",  "action": "preset5",  "mod_action": "savepreset5" },
    { "index": 5,  "name": "pad6",  "action": "preset6",  "mod_action": "savepreset6" },
    { "index": 6,  "name": "pad7",  "action": "preset7",  "mod_action": "savepreset7" },
    { "index": 7,  "name": "pad8",  "action": "preset8",  "mod_action": "savepreset8" },
    { "index": 8,  "name": "pad9",  "action": "preset9",  "mod_action": "savepreset9" },
    { "index": 9,  "name": "pad10", "action": "preset10", "mod_action": "savepreset10" },
    { "index": 10, "name": "pad11", "action": "preset11", "mod_action": "savepreset11" },
    { "index": 11, "name": "pad12", "action": "preset12", "mod_action": "savepreset12" },
    { "index": 12, "name": "pad13", "action": "preset13", "mod_action": "savepreset13" },
    { "index": 13, "name": "pad14", "action": "preset14", "mod_action": "savepreset14" },
    { "index": 14, "name": "pad15", "action": "preset15", "mod_action": "savepreset15" },
    { "index": 15, "name": "pad16", "action": "preset16", "mod_action": "savepreset16" },
    { "index": 16, "name": "shift", "modifier": true },
    { "index": 17, "name": "reverse", "action": "prevpal" },
    { "index": 18, "name": "type",    "action": "nextpreset" },
    { "index": 19, "name": "size",    "action": "prevpreset" },
    { "index": 20, "name": "browse",  "action": "blackout" },
    { "index": 21, "name": "encoder", "action": "full" },
    { "index": 24, "name": "bar1", "action": "seg0" },
    { "index": 25, "name": "bar2", "action": "seg1" },
    { "index": 26, "name": "bar3", "action": "seg2" },
    { "index": 27, "name": "bar4", "action": "seg3" },
    { "index": 28, "name": "sync",    "action": "nextfx" },
    { "index": 29, "name": "quant",   "action": "prevfx" },
    { "index": 30, "name": "capture", "action": "nextpal" }
  ]
}
)JSON";

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

// The Soomfon CN003 - and the rest of the OpenDeck family.
//
// It is not a plain HID device. Its input reports are 512 bytes and each
// one is a SINGLE EVENT: "ACK" at byte 0, the message ID at byte 9 and the
// state at byte 10. Its output is the ASCII "CRT" command protocol, every
// command zero-padded to 1025 bytes.
//
// The message IDs are the ones opendeck-akp05 uses:
//
//   0x01..0x0A   the ten keys with displays (button indices 0..9)
//   0x00, 0x40..0x43  the five buttons without displays (indices 11..15)
//   0x37, 0x35, 0x33, 0x36   the four encoders' push buttons
//   0xa0/0xa1, 0x50/0x51, 0x90/0x91, 0x70/0x71   the four encoders,
//               twisted: the low bit of the ID is the direction
//
// The touch strip's four zones are the same 0x40..0x43 messages.
//
// The encoders: the first one pages the presets, the fourth drives the
// global brightness, and the second and the third drive the effect's own
// controls - a touch on the strip above them picks which one, and a
// press on one of them turns a check box on and off. The wide strip
// shows what each of them does.
//
// The page is ten slots - one for each of the keys with a screen - so
// the pages and the keys agree.
//
// The wide touch strip is driven by four zone pictures, one per encoder,
// which the deck puts side by side on its one screen. The one-picture
// command across the whole strip ("bgpic") is the reference's other way
// and this deck does not take it: its strip stayed black.
static const char HIDCTL_CN003_PROFILE_JSON[] = R"JSON({
  "name": "Soomfon CN003",
  "vid": "0x1500",
  "pid": "0x3002",
  "kind": "opendeck",
  "report_id": 0,
  "report_len": 512,
  "page_size": 10,
  "opendeck": { "keys": 10, "encoders": 4, "bgpic": false },
  "encoders": [
    { "name": "enc1", "action": "page", "push_action": "showpage" },
    { "name": "enc2", "action": "effectSpeed", "push_action": "ctrlbri" },
    { "name": "enc3", "action": "effectIntensity", "push_action": "" },
    { "name": "enc4", "action": "bri", "push_action": "" }
  ],
  "button_map": [
    { "index": 0,  "name": "key1",  "action": "preset1" },
    { "index": 1,  "name": "key2",  "action": "preset2" },
    { "index": 2,  "name": "key3",  "action": "preset3" },
    { "index": 3,  "name": "key4",  "action": "preset4" },
    { "index": 4,  "name": "key5",  "action": "preset5" },
    { "index": 5,  "name": "key6",  "action": "preset6" },
    { "index": 6,  "name": "key7",  "action": "preset7" },
    { "index": 7,  "name": "key8",  "action": "preset8" },
    { "index": 8,  "name": "key9",  "action": "preset9" },
    { "index": 9,  "name": "key10", "action": "preset10" },
    { "index": 11, "name": "zone1", "action": "nextpreset" },
    { "index": 12, "name": "zone2", "action": "prevpreset" },
    { "index": 13, "name": "zone3", "action": "nextctrl" },
    { "index": 14, "name": "zone4", "action": "nextctrl" },
    { "index": 15, "name": "zone5", "action": "blackout" }
  ]
})JSON";

static const char HIDCTL_DEFAULT_PROFILE_JSON[] = R"JSON({
  "name": "Traktor Kontrol F1",
  "vid": "0x17CC",
  "pid": "0x1120",
  "report_id": 1,
  "report_len": 22,
  "buttons": { "bytes": [1, 2, 3, 4], "msb_first": true },
  "wheel": { "offset": 5 },
  "controls": [
    { "name": "knob1",  "offset": 6,  "bits": 12, "action": "bri" },
    { "name": "knob2",  "offset": 8,  "bits": 12, "action": "effectSpeed" },
    { "name": "knob3",  "offset": 10, "bits": 12, "action": "effectIntensity" },
    { "name": "knob4",  "offset": 12, "bits": 12, "action": "effectPalette", "mod_action": "ctrlbri" },
    { "name": "fader1", "offset": 14, "bits": 12, "action": "seg0bri" },
    { "name": "fader2", "offset": 16, "bits": 12, "action": "seg1bri" },
    { "name": "fader3", "offset": 18, "bits": 12, "action": "seg2bri" },
    { "name": "fader4", "offset": 20, "bits": 12, "action": "seg3bri" }
  ],
  "button_map": [
    { "index": 0,  "name": "pad1",  "action": "preset1",  "mod_action": "preset17" },
    { "index": 1,  "name": "pad2",  "action": "preset2",  "mod_action": "preset18" },
    { "index": 2,  "name": "pad3",  "action": "preset3",  "mod_action": "preset19" },
    { "index": 3,  "name": "pad4",  "action": "preset4",  "mod_action": "preset20" },
    { "index": 4,  "name": "pad5",  "action": "preset5",  "mod_action": "preset21" },
    { "index": 5,  "name": "pad6",  "action": "preset6",  "mod_action": "preset22" },
    { "index": 6,  "name": "pad7",  "action": "preset7",  "mod_action": "preset23" },
    { "index": 7,  "name": "pad8",  "action": "preset8",  "mod_action": "preset24" },
    { "index": 8,  "name": "pad9",  "action": "preset9",  "mod_action": "preset25" },
    { "index": 9,  "name": "pad10", "action": "preset10", "mod_action": "preset26" },
    { "index": 10, "name": "pad11", "action": "preset11", "mod_action": "preset27" },
    { "index": 11, "name": "pad12", "action": "preset12", "mod_action": "preset28" },
    { "index": 12, "name": "pad13", "action": "preset13", "mod_action": "preset29" },
    { "index": 13, "name": "pad14", "action": "preset14", "mod_action": "preset30" },
    { "index": 14, "name": "pad15", "action": "preset15", "mod_action": "preset31" },
    { "index": 15, "name": "pad16", "action": "preset16", "mod_action": "preset32" },
    { "index": 16, "name": "shift", "modifier": true },
    { "index": 17, "name": "reverse", "action": "nextpreset" },
    { "index": 18, "name": "type",    "action": "nextfx" },
    { "index": 19, "name": "size",    "action": "prevpreset" },
    { "index": 20, "name": "browse",  "action": "blackout" },
    { "index": 21, "name": "encoder", "action": "showpage" },
    { "index": 24, "name": "bar1", "action": "downbeat", "mod_action": "seg0" },
    { "index": 25, "name": "bar2", "action": "", "mod_action": "seg1" },
    { "index": 26, "name": "bar3", "action": "", "mod_action": "seg2" },
    { "index": 27, "name": "bar4", "action": "", "mod_action": "seg3" },
    { "index": 28, "name": "sync",    "action": "nextfx" },
    { "index": 29, "name": "quant",   "action": "prevpreset" },
    { "index": 30, "name": "capture", "action": "prevfx" }
  ],
  "output": {
    "report_id": 128,
    "length": 81,
    "display": {
      "digits": [ { "offset": 9 }, { "offset": 1 } ],
      "segments_per_digit": 8,
      "segments": ["dot", "middle", "lower-right", "upper-right",
                   "top", "upper-left", "lower-left", "bottom"]
    },
    "lights": [
      { "name": "browse",  "offset": 17, "bytes": 1 },
      { "name": "size",    "offset": 18, "bytes": 1 },
      { "name": "type",    "offset": 19, "bytes": 1 },
      { "name": "reverse", "offset": 20, "bytes": 1 },
      { "name": "shift",   "offset": 21, "bytes": 1 },
      { "name": "capture", "offset": 22, "bytes": 1 },
      { "name": "quant",   "offset": 23, "bytes": 1 },
      { "name": "sync",    "offset": 24, "bytes": 1 },

      { "name": "pad1",  "offset": 25, "bytes": 3 },
      { "name": "pad2",  "offset": 28, "bytes": 3 },
      { "name": "pad3",  "offset": 31, "bytes": 3 },
      { "name": "pad4",  "offset": 34, "bytes": 3 },
      { "name": "pad5",  "offset": 37, "bytes": 3 },
      { "name": "pad6",  "offset": 40, "bytes": 3 },
      { "name": "pad7",  "offset": 43, "bytes": 3 },
      { "name": "pad8",  "offset": 46, "bytes": 3 },
      { "name": "pad9",  "offset": 49, "bytes": 3 },
      { "name": "pad10", "offset": 52, "bytes": 3 },
      { "name": "pad11", "offset": 55, "bytes": 3 },
      { "name": "pad12", "offset": 58, "bytes": 3 },
      { "name": "pad13", "offset": 61, "bytes": 3 },
      { "name": "pad14", "offset": 64, "bytes": 3 },
      { "name": "pad15", "offset": 67, "bytes": 3 },
      { "name": "pad16", "offset": 70, "bytes": 3 },

      { "name": "bar1", "offset": 79, "bytes": 2 },
      { "name": "bar2", "offset": 77, "bytes": 2 },
      { "name": "bar3", "offset": 75, "bytes": 2 },
      { "name": "bar4", "offset": 73, "bytes": 2 }
    ]
  }
}
)JSON";

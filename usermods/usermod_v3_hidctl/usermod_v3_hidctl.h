// usermod_v3_hidctl.h
//
// Generic USB HID control surface for WLED (ESP32-P4).
//
// A "profile" describes one controller: how to recognise it (VID/PID), how
// to read its input reports, and what each control does in WLED. Profiles
// are plain JSON files on the filesystem, one file per device, in
// /hidctl/*.json — they are NOT part of cfg.json, so a controller can be
// added, changed or removed without touching the runtime configuration and
// without rebuilding the firmware.
//
// The Traktor Kontrol F1 profile ships with the firmware (it is written to
// /hidctl/traktor-kontrol-f1.json on first boot if that file is missing)
// and is a starting point for other HID controllers.
//
// Profile format:
//
// {
//   "name":     "Traktor Kontrol F1",
//   "vid":      "0x17CC",
//   "pid":      "0x1120",
//   "report_id": 1,            // optional: only accept this report ID
//   "report_len": 22,          // bytes in the report, including the ID byte
//   "buttons":  { "bytes": [1,2,3,4], "msb_first": true },
//   "controls": [
//     { "name": "knob1", "offset": 6, "bits": 12, "action": "bri" },
//     { "name": "knob2", "offset": 8, "bits": 12, "action": "effectSpeed" }
//   ],
//   "button_map": [
//     { "index": 0, "name": "pad1", "action": "preset1",
//       "mod_action": "savepreset1" },      // with a modifier held
//     { "index": 16, "name": "shift", "modifier": true }
//   ]
// }
//
// A control is a little-endian unsigned integer of "bits" bits at "offset"
// in the report. A button is one bit of the button bitmap bytes; the
// bitmap's bits are numbered MSB first within each byte, and the bytes are
// concatenated in the order given, so button index 0 is the top bit of the
// first byte.
//
// Action vocabulary (see runAction()):
//   parameters  bri, effectSpeed, effectIntensity, effectPalette,
//               effectCustom1, effectCustom2, seg0bri .. seg7bri
//   verbs       power, blackout, full, nextfx, prevfx, nextpal, prevpal,
//               nextpreset, prevpreset, toggleFreeze, seg0 .. seg7
//   presets     preset1 .. preset64, savepreset1 .. savepreset64

#pragma once

#include "wled.h"
#include "hidctl_types.h"
#include "hidctl_default_profile.h"
#include "hid_usb_host.h"
// The OpenDeck-family protocol: the ASCII "CRT" commands and the input
// report's message IDs. A port of `ambiso/mirajazz` (the crate the
// OpenDeck plugin links against) and `ambiso/opendeck-akp05`.
#include "opendeck_proto.h"

// The P4 has a hardware JPEG encoder, which is what makes the deck's
// pictures possible. On any other target there is no encoder, so the
// pictures are simply not pushed.
#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(SOC_JPEG_ENCODE_SUPPORTED)
  #include "driver/jpeg_encode.h"
  #include <esp_cache.h>
  #include <esp_heap_caps.h>
  #define HIDCTL_HAS_JPEG 1
#else
  #define HIDCTL_HAS_JPEG 0
#endif

// The tempo tracker's published frame (the beat in the bar, the downbeat
// state). ar_publish.h is a light header - just the structs.
#include "ar_publish.h"

#ifdef USERMOD_AUDIOREACTIVE
// The tempo tracker's serial keys. 'j' sets the downbeat (the beat
// sounding now is beat one), 'J' clears the mark, 'q' re-locks the grid.
// Defined in usermods/audioreactive/ar_tempo.h, which the AudioReactive
// usermod includes.
void audioReactiveTempoCommand(char c);
#endif

// The AutoPlaylist's music slot (0 when there is none). Defined in
// usermods_list.cpp, because auto_playlist.h cannot be included in more
// than one translation unit.
uint8_t queryAutoPlaylistMusicSlot();

class HidctlUsermod : public Usermod {
 private:
  static constexpr const char _name[]    PROGMEM = "HIDCTL";
  static constexpr const char _key_enabled[] PROGMEM = "enabled";

  bool enabled = true;

  // Profiles live in PSRAM when it is available: they are read once at boot
  // and then sit there untouched, so there is no reason to spend internal
  // RAM on them.
  HidctlProfile* profiles = nullptr;
  uint8_t        profile_count = 0;

  // The profile currently bound to the connected device (index into
  // `profiles`), or 0xFF.
  uint8_t active_profile = 0xFF;

  // Decoded state of the connected device.
  uint16_t control_value[HIDCTL_MAX_CONTROLS] = {0};
  uint8_t  control_taken[HIDCTL_MAX_CONTROLS]  = {0};
  uint32_t buttons = 0;
  uint32_t modifier_mask = 0;

  // -------------------------------------------------------------------------
  // The continuous controls, and how often they are allowed to drive WLED
  // -------------------------------------------------------------------------
  // A knob that nobody is touching still jitters by a count or two, and a
  // knob being swept reports its new position every few milliseconds.
  // Neither should reach WLED at the report rate: each one costs a segment
  // re-apply, a state broadcast and an interface push. So:
  //
  //   control_applied     the raw value last driven into WLED - the
  //                       baseline the deadband is measured against
  //   control_mapped      the 0..255 value that produced, so an unchanged
  //                       result is not driven again
  //   control_pending     a newer value is waiting for the throttle
  //   control_has_applied the baseline is meaningful
  //   control_waited      the takeover wait has already been said once
  //   control_applied_ms  when it was last driven
  //   control_log_ms      when it was last written to the console
  uint16_t control_applied[HIDCTL_MAX_CONTROLS]     = {0};
  uint8_t  control_mapped[HIDCTL_MAX_CONTROLS]      = {0};
  bool     control_has_applied[HIDCTL_MAX_CONTROLS] = {false};
  bool     control_pending[HIDCTL_MAX_CONTROLS]      = {false};
  bool     control_waited[HIDCTL_MAX_CONTROLS]       = {false};
  uint32_t control_applied_ms[HIDCTL_MAX_CONTROLS]   = {0};
  uint32_t control_log_ms[HIDCTL_MAX_CONTROLS]      = {0};

  // A change smaller than this many raw counts is not a movement. A 12-bit
  // knob jitters by one or two counts, so four takes that out.
  static constexpr uint16_t CONTROL_DEADBAND = 4;

  // How often a control may drive WLED, even when it is being swept fast.
  // The report arrives about a hundred times a second; this is the rate the
  // board actually works at.
  static constexpr uint32_t CONTROL_APPLY_INTERVAL_MS = 50;

  // How often a moving control is written to the console.
  static constexpr uint32_t CONTROL_LOG_INTERVAL_MS = 500;

  // Forget everything the controls were: the first report after a connect
  // drives them all again.
  void resetControls() {
    for (uint8_t i = 0; i < HIDCTL_MAX_CONTROLS; i++) {
      control_value[i] = 0xFFFF;   // force the next report to apply
      control_taken[i] = 0;
      control_applied[i] = 0;
      control_mapped[i] = 0;
      control_has_applied[i] = false;
      control_pending[i] = false;
      control_waited[i] = false;
      control_applied_ms[i] = 0;
      control_log_ms[i] = 0;
    }
  }

  // Websocket push throttle while a control is moving.
  uint32_t lastWsPush = 0;
  static constexpr uint32_t WS_PUSH_INTERVAL_MS = 250;

  // The beat that is sounding (1..4), from the tempo tracker. The lower
  // buttons show it.
  uint8_t currentBeat = 0;

  // The AutoPlaylist's music slot, for the pad colours.
  uint8_t music_slot = 0;

  // -------------------------------------------------------------------------
  // The reports that arrive
  // -------------------------------------------------------------------------
  // The profile asks for one report ID, but a device may send others, and
  // the F1 declares three input report IDs (0x01, 0xF0 and 0xD0). They are
  // recorded here - one entry per report ID, with the latest bytes and how
  // many have arrived - so a report that is being thrown away can still be
  // seen.
  static constexpr uint8_t HIDCTL_SEEN_REPORTS = 8;
  static constexpr uint8_t HIDCTL_SEEN_BYTES = 32;
  struct SeenReport {
    uint8_t  id;
    uint8_t  len;
    uint32_t count;
    uint8_t  bytes[HIDCTL_SEEN_BYTES];
  };
  SeenReport seen[HIDCTL_SEEN_REPORTS] = {};
  uint8_t    seen_count = 0;

  // The wheel byte's lowest and highest value ever seen. "It never moves"
  // is then a number, not a belief.
  uint8_t wheel_min = 0xFF;
  uint8_t wheel_max = 0;
  bool    wheel_moved = false;

  // Record a report: one entry per report ID, keeping the latest bytes
  // and counting how many have arrived.
  void recordReport(const uint8_t* data, uint16_t len) {
    if (!data || len == 0) return;
    uint8_t id = data[0];
    SeenReport* s = nullptr;
    for (uint8_t i = 0; i < seen_count; i++) {
      if (seen[i].id == id) { s = &seen[i]; break; }
    }
    if (!s) {
      if (seen_count >= HIDCTL_SEEN_REPORTS) return;
      s = &seen[seen_count++];
      s->id = id;
      s->len = 0;
      s->count = 0;
    }
    s->count++;
    s->len = (uint8_t)(len > 255 ? 255 : len);
    uint16_t n = (len > HIDCTL_SEEN_BYTES) ? HIDCTL_SEEN_BYTES : len;
    memcpy(s->bytes, data, n);
  }

  // The preset that is active, tracked across the core's wiping of
  // currentPreset. The core sets currentPreset back to 0 as soon as
  // anything changes ("something changed, so we are no longer in the
  // preset" - wled00/led.cpp), which would take the green highlight off
  // the active pad the moment a knob moved. The core's onPreStateChange
  // hook fires before that wipe, so the value is latched here instead.
  uint8_t tracked_active_preset = 0;

  // -------------------------------------------------------------------------
  // The preset pages
  // -------------------------------------------------------------------------
  // The presets are paged in blocks of sixteen: the pads address the
  // current page, and the encoder turns the page. Turning it clockwise
  // goes forward a page and anticlockwise back; it never wraps - the
  // first page stays the first and the last page stays the last.
  static constexpr uint8_t PAGE_SIZE = 16;

  uint8_t  page = 1;          // the page the pads are addressing
  uint8_t  wheelLast = 0;     // the encoder's last position
  bool     wheelSeen = false;
  uint32_t showPageUntil = 0; // the page is on the display until then
  uint32_t showWheelUntil = 0; // the encoder's position is on the display until then

  // The last page a preset can live on. The pages are not limited to the
  // presets that happen to be saved: the user browses ahead of them and
  // saves there.
  static constexpr uint8_t LAST_PAGE = (250 + PAGE_SIZE - 1) / PAGE_SIZE;

  uint8_t lastPage() const {
    return LAST_PAGE;
  }

  // The page the current preset is in.
  uint8_t pageOfPreset(uint16_t preset) const {
    if (preset < 1) preset = 1;
    return (uint8_t)(((preset - 1) / PAGE_SIZE) + 1);
  }

  // The offset the pads are shifted by: page one is the first sixteen
  // presets, page two the next sixteen, and so on.
  uint16_t pageOffset() const {
    return (uint16_t)((page - 1) * PAGE_SIZE);
  }

  // Move a page forward (1) or back (-1).
  //
  // A page is sixteen SLOTS, not sixteen presets: some of them may have
  // nothing saved on them. The page is what the pads address.
  //
  // Turning the page does NOT touch the running preset. The preset moves
  // only when it is changed from outside the controller - and then the
  // page jumps to the preset (onEvent's PresetApplied), not the other way
  // round.
  void turnPage(int direction) {
    const uint8_t last = lastPage();
    int want = (int)page + direction;
    if (want < 1) want = 1;
    if (want > last) want = last;
    if (want == (int)page) return;
    page = (uint8_t)want;
    showPageUntil = millis() + 1500;
    USER_PRINTF("[HIDCTL] page %u\n", page);
    // The page is on the display for a moment, and the pads move to the
    // new page's slots whether or not a preset is saved on them.
    paint();
    // A deck's screens show the page's slots, so they change with it.
    repaintDeck();
  }

  // Show the page on the display for a moment.
  void showPageBriefly() {
    showPageUntil = millis() + 1500;
  }

  // The LED demo: a chase of brightnesses, a rainbow over the pads and a
  // counter on the display. It is switched on and off at runtime, so it
  // can be seen without rebuilding:
  //
  //   curl -X POST http://<board>/json/state -d '{"HIDCTL":{"demo":true}}'
  //
  bool demoActive = false;
  uint32_t demoStart = 0;
  bool paint_enabled = true;
  // The LED report goes on the interrupt OUT endpoint. It is 81 bytes and
  // that endpoint takes 64, but the F1 takes it there and nowhere else -
  // and the control pipe is no use anyway: the F1 stalls SET_IDLE and a
  // stalled control endpoint stays stalled.
  bool led_out_endpoint = true;

  // Is this preset slot a saved playlist?
  static bool presetIsPlaylist(uint8_t slot) {
    if (slot == 0 || slot > 250) return false;
    String name;
    bool isPlaylist = false;
    if (!getCachedPresetMetadata(slot, name, isPlaylist)) return false;
    return isPlaylist;
  }

  // The last output-report attempts and their status, published over
  // /json/ar so they can be read without the console.
  static constexpr uint8_t HIDCTL_MAX_PROBE_RESULTS = 12;
  struct ProbeResult { uint8_t type; uint8_t id; uint16_t len; int8_t status; };
  ProbeResult probe_results[HIDCTL_MAX_PROBE_RESULTS] = {};
  uint8_t probe_result_count = 0;

  // -------------------------------------------------------------------------
  // who owns the USB
  // -------------------------------------------------------------------------
  // The USB host stack belongs to ONE task: the main loop, which calls
  // hid_usb_poll() and the class requests. Everything else - the event
  // bus's handlers (they run in the publisher's task), the core's state
  // change callback, the web API - must not touch it.
  //
  // Submitting a transfer from another task corrupts the host stack: the
  // transfer is not reentrant, and two tasks submitting it at once is how
  // the board ended up with a null-pointer store in the USB task.
  //
  // So the LED report is BUILT wherever it is convenient and SENT by
  // loop(), and the runtime commands that touch USB are carried there too.
  uint8_t led_report[HIDCTL_MAX_OUTPUT] = {};
  uint16_t led_report_len = 0;
  bool led_report_pending = false;
  bool cmd_wakeup = false;        // send the wake-up packet
  int8_t cmd_other_iface = -1;    // 0 = release the other interface, 1 = claim

  // The BROWSE button is a latch: the room is dark, or it is not.
  bool blackout_latched = false;
  uint8_t bri_before_blackout = 255;
  // The controller's own button brightness (SHIFT + knob4). Never zero:
  // the pads have to stay readable enough to see the active preset.
  static constexpr uint8_t CTRL_BRI_MIN = 26;   // about a tenth
  uint8_t ctrl_bri = 255;

  // -------------------------------------------------------------------------
  // blinking
  // -------------------------------------------------------------------------
  // A light - or the two-digit display - that alternates at a rate for a
  // while. The controller has no other way to say "that happened".
  //
  //   blink("sync", 2.0f, 3000);   // the SYNC button, twice a second, 3 s
  //   blink("", 4.0f, 500);         // the display, four times a second
  //
  // An empty name means the display. A light keeps its own colour in the
  // "off" half of the cycle and is white in the "on" half.
  static constexpr uint8_t HIDCTL_MAX_BLINKS = 6;
  struct Blink {
    char     name[HIDCTL_MAX_NAME];   // the light's name; "" = the display
    float    rate;                     // Hz
    uint32_t started_ms;
    uint32_t until_ms;
    bool     keep_colour;              // true: the light keeps its colour and
                                        //       goes dark in the off half
  };
  Blink blinks[HIDCTL_MAX_BLINKS] = {};
  uint8_t blink_count = 0;

 public:
  // Start a blink. A new one for the same light replaces the old one; one
  // that is already running keeps its phase and is only extended, so a
  // chaser that refreshes itself every paint does not stutter.
  //
  // keep_colour: the light keeps its own colour and goes dark in the off
  // half (the playlist chaser). Otherwise it flashes white.
  void blink(const char* name, float rate_hz, uint32_t duration_ms,
             bool keep_colour = false) {
    if (blinkSet(name, rate_hz, duration_ms, keep_colour)) paint();
  }

  // Set the blink WITHOUT painting. The paint itself uses this - painting
  // from inside the paint would recurse.
  bool blinkSet(const char* name, float rate_hz, uint32_t duration_ms,
                bool keep_colour = false) {
    if (rate_hz <= 0.0f || duration_ms == 0) return false;
    const char* want = name ? name : "";
    // Drop the blinks that have run out, so the slots are free again.
    const uint32_t now = millis();
    uint8_t w = 0;
    for (uint8_t i = 0; i < blink_count; i++) {
      if ((int32_t)(blinks[i].until_ms - now) > 0) {
        if (w != i) blinks[w] = blinks[i];
        w++;
      }
    }
    blink_count = w;

    uint8_t slot = blink_count;
    bool fresh = true;
    for (uint8_t i = 0; i < blink_count; i++) {
      if (!strcmp(blinks[i].name, want)) { slot = i; fresh = false; break; }
    }
    if (slot >= HIDCTL_MAX_BLINKS) return false;
    if (slot == blink_count) blink_count++;
    copyStr(blinks[slot].name, want);
    blinks[slot].rate = rate_hz;
    blinks[slot].keep_colour = keep_colour;
    if (fresh) blinks[slot].started_ms = now;
    blinks[slot].until_ms = now + duration_ms;
    return true;
  }

  // Is anything blinking right now? While it is, the LEDs are repainted
  // faster than the usual ten times a second, so a fast blink is fast.
  bool blinkActive() const {
    const uint32_t now = millis();
    for (uint8_t i = 0; i < blink_count; i++) {
      if ((int32_t)(blinks[i].until_ms - now) > 0) return true;
    }
    return false;
  }

  // Where this light (or the display, for an empty name) is in its cycle:
  // -1 = not blinking, 0 = the off half, 1 = the on half.
  int blinkPhase(const char* name) const {
    const uint32_t now = millis();
    const char* want = name ? name : "";
    for (uint8_t i = 0; i < blink_count; i++) {
      if (strcmp(blinks[i].name, want)) continue;
      if ((int32_t)(blinks[i].until_ms - now) <= 0) continue;
      uint32_t period_ms = (uint32_t)(1000.0f / blinks[i].rate);
      if (period_ms < 4) period_ms = 4;          // the half-period must not be 0
      uint32_t half = period_ms / 2;
      return (int)((((now - blinks[i].started_ms) / half) % 2) == 0);
    }
    return -1;
  }

  // Does this light keep its own colour while it blinks?
  bool blinkKeepsColour(const char* name) const {
    const char* want = name ? name : "";
    for (uint8_t i = 0; i < blink_count; i++) {
      if (!strcmp(blinks[i].name, want)) return blinks[i].keep_colour;
    }
    return false;
  }

 private:

  // -------------------------------------------------------------------------
  // profile handling
  // -------------------------------------------------------------------------
  static void copyStr(char (&dst)[HIDCTL_MAX_NAME], const char* src) {
    if (!src) src = "";
    strlcpy(dst, src, sizeof(dst));
  }
  static void copyAction(char (&dst)[HIDCTL_MAX_ACTION], const char* src) {
    if (!src) src = "";
    strlcpy(dst, src, sizeof(dst));
  }

  static bool parseHex(const char* s, uint16_t& out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    unsigned long v = strtoul(s, &end, 0);   // 0x prefix aware
    if (end == s) return false;
    out = (uint16_t)v;
    return true;
  }

  // Parse one profile JSON object into `p`.
  static bool parseProfile(JsonObject& j, HidctlProfile& p) {
    memset(&p, 0, sizeof(p));

    const char* nm = j["name"];
    copyStr(p.name, nm);

    uint16_t vid = 0, pid = 0;
    if (!parseHex(j["vid"], vid)) return false;
    if (!parseHex(j["pid"], pid)) return false;
    p.vid = vid;
    p.pid = pid;

    p.report_id = (uint8_t)(j["report_id"] | 0);
    p.report_len = (uint16_t)(j["report_len"] | 0);
    if (p.report_len == 0 || p.report_len > HIDCTL_MAX_REPORT) return false;

    // The kind of device. "opendeck" is the OpenDeck-family protocol: the
    // input is single events with a message ID, and the output is the
    // ASCII "CRT" command protocol rather than an LED report.
    {
      const char* kind = j["kind"] | "";
      p.opendeck = (kind && !strcmp(kind, "opendeck"));
      JsonObject od = j["opendeck"];
      if (!od.isNull()) {
        // How many screens and encoders this deck has, and whether its
        // wide touch strip is one screen (a "BGPIC" picture across the
        // whole strip) or four separate zones.
        p.od_keys       = (uint8_t)(od["keys"] | 10);
        p.od_encoders   = (uint8_t)(od["encoders"] | 4);
        p.od_strip_slot = (uint8_t)(od["strip_slot"] | 0);
        p.od_strip_one  = od["strip_one"] | false;
      }
    }

    // Button bitmap bytes.
    JsonObject bt = j["buttons"];
    JsonArray bytes = bt["bytes"];
    p.button_byte_count = 0;
    for (JsonVariant v : bytes) {
      if (p.button_byte_count >= HIDCTL_MAX_BUTTON_BYTES) break;
      uint16_t off = (uint16_t)(v.as<int>());
      // The button bytes are read out of the report, so they have to be
      // inside it.
      if (off >= p.report_len) {
        USER_PRINTF("[HIDCTL] button byte %u is outside the %u-byte report\n",
                    off, p.report_len);
        continue;
      }
      p.button_bytes[p.button_byte_count++] = (uint8_t)off;
    }
    p.msb_first = bt["msb_first"] | true;

    // Continuous controls.
    JsonArray ctrls = j["controls"];
    p.control_count = 0;
    for (JsonObject c : ctrls) {
      if (p.control_count >= HIDCTL_MAX_CONTROLS) break;
      HidctlControl& ctl = p.controls[p.control_count];
      const char* cn = c["name"];
      copyStr(ctl.name, cn);
      ctl.offset = (uint16_t)(c["offset"] | 0);
      ctl.bits = (uint8_t)(c["bits"] | 12);
      const char* act = c["action"];
      copyAction(ctl.action, act);
      // What the control drives while a modifier is held.
      const char* mact = c["mod_action"];
      copyAction(ctl.mod_action, mact);
      if (ctl.bits == 0 || ctl.bits > 16) return false;
      if (ctl.offset + ((ctl.bits + 7) / 8) > p.report_len) return false;
      p.control_count++;
    }

    // The rotary encoders of an OpenDeck-family deck. They report a twist
    // of plus or minus one and a push, not a position.
    JsonArray encs = j["encoders"];
    p.encoder_count = 0;
    for (JsonObject e : encs) {
      if (p.encoder_count >= HIDCTL_MAX_ENCODERS) break;
      HidctlEncoder& enc = p.encoders[p.encoder_count];
      const char* en = e["name"];
      copyStr(enc.name, en);
      const char* act = e["action"];
      copyAction(enc.action, act);
      const char* push = e["push_action"];
      copyAction(enc.push_action, push);
      enc.value = 0;
      p.encoder_count++;
    }

    // Buttons.
    JsonArray bmap = j["button_map"];
    p.button_count = 0;
    for (JsonObject b : bmap) {
      if (p.button_count >= HIDCTL_MAX_BUTTONS) break;
      HidctlButton& btn = p.buttons[p.button_count];
      btn.index = (uint8_t)(b["index"] | 0);
      // The button bitmap is 32 bits wide, so a bit index of 32 or more
      // has nowhere to go - and shifting by that much is undefined.
      if (btn.index >= 32) {
        const char* bn = b["name"];
        USER_PRINTF("[HIDCTL] button %s: index %u is outside the 32-bit bitmap\n",
                    bn ? bn : "?", btn.index);
        continue;
      }
      const char* bn = b["name"];
      copyStr(btn.name, bn);
      const char* act = b["action"];
      copyAction(btn.action, act);
      const char* mod = b["mod_action"];
      copyAction(btn.mod_action, mod);
      btn.modifier = b["modifier"] | false;
      p.button_count++;
    }

    // The wheel: a byte in the INPUT report that counts the encoder's
    // position, so a difference tells which way it turned. It describes the
    // input, so it is read from the top level of the profile: a profile
    // without an "output" section still has a wheel.
    JsonObject wheel = j["wheel"];
    if (!wheel.isNull()) {
      p.wheel_present = true;
      p.wheel_offset = (uint16_t)(wheel["offset"] | 0);
      if (p.wheel_offset >= p.report_len) return false;
    }

    // The output report: LEDs and the display.
    JsonObject out = j["output"];
    if (!out.isNull()) {
      p.output.present = true;
      p.output.report_id = (uint8_t)(out["report_id"] | 0x80);
      p.output.length = (uint16_t)(out["length"] | 0);
      if (p.output.length == 0 || p.output.length > HIDCTL_MAX_OUTPUT) return false;

      JsonObject disp = out["display"];
      if (!disp.isNull()) {
        p.output.display.present = true;
        JsonArray digits = disp["digits"];
        uint8_t dc = 0;
        for (JsonObject d : digits) {
          if (dc >= HIDCTL_MAX_DIGITS) break;
          p.output.display.digit_offset[dc] = (uint16_t)(d["offset"] | 0);
          dc++;
        }
        p.output.display.digit_count = dc;
        int dot = disp["dot_offset"] | -1;
        p.output.display.dot_offset = (dot < 0) ? 0xFFFF : (uint16_t)dot;
        JsonArray segs = disp["segments"];
        uint8_t n = 0;
        for (JsonVariant v : segs) {
          if (n >= HIDCTL_MAX_SEGMENTS) break;
          const char* s = v;
          strlcpy(p.output.display.segment_names[n], s ? s : "", HIDCTL_MAX_SEGMENTS + 4);
          n++;
        }
        p.output.display.segments_per_digit = n;
        if (n == 0) p.output.display.present = false;
      }

      JsonArray lights = out["lights"];
      uint8_t lc = 0;
      for (JsonObject l : lights) {
        if (lc >= HIDCTL_MAX_LIGHTS) break;
        HidctlLight& li = p.output.lights[lc];
        const char* nm = l["name"];
        copyStr(li.name, nm);
        li.offset = (uint16_t)(l["offset"] | 0);
        li.bytes = (uint8_t)(l["bytes"] | 1);
        if (li.bytes < 1 || li.bytes > 3) li.bytes = 1;
        if (li.offset + li.bytes > p.output.length) return false;
        lc++;
      }
      p.output.light_count = lc;
    }
    return true;
  }

  // The profile that ships with the firmware: the Traktor Kontrol F1.
  // Written to the filesystem on first boot so it can be read, edited and
  // copied like any other profile.
  static const char* defaultProfileText() { return HIDCTL_DEFAULT_PROFILE_JSON; }

  void writeDefaultProfileIfMissing(const char* filename, const char* content) {
    if (!WLED_FS.exists(filename)) writeDefaultProfile(filename, content);
  }

  // Write a profile over whatever is there. The defaults are only written
  // when the file is missing, so a profile that was written by an older
  // firmware keeps its old contents until it is written again - which is
  // what the "writedefaults" command from the web API does.
  void writeDefaultProfile(const char* filename, const char* content) {
    File f = WLED_FS.open(filename, "w");
    if (!f) {
      USER_PRINTF("[HIDCTL] could not write default profile %s\n", filename);
      return;
    }
    f.print(content);
    f.close();
    USER_PRINTF("[HIDCTL] wrote default profile %s\n", filename);
  }

  // Write both built-in profiles over the ones on the filesystem and read
  // them back.
  void writeDefaultProfiles() {
    writeDefaultProfile(HIDCTL_PROFILE_DIR HIDCTL_DEFAULT_PROFILE_FILE,
                        defaultProfileText());
    writeDefaultProfile(HIDCTL_PROFILE_DIR "hidctl-soomfon-cn003.json",
                        HIDCTL_CN003_PROFILE_JSON);
    loadProfiles();
  }

  // Load every profile in /hidctl/*.json.
  void loadProfiles() {
    if (!profiles) {
      // Prefer PSRAM: internal RAM is scarce and the profiles are read
      // once at boot and then sit there untouched.
      profiles = (HidctlProfile*)heap_caps_calloc_prefer(
          sizeof(HidctlProfile) * HIDCTL_MAX_PROFILES, 1, 2,
          MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL);
    }
    if (!profiles) {
      USER_PRINTLN("[HIDCTL] out of memory for profiles");
      return;
    }
    memset(profiles, 0, sizeof(HidctlProfile) * HIDCTL_MAX_PROFILES);
    profile_count = 0;

    // Profiles live in the LittleFS root. This FS has no openDir(), so the
    // root is opened as a file and walked with openNextFile().
    File dir = WLED_FS.open(HIDCTL_PROFILE_DIR);
    if (!dir) {
      USER_PRINTLN("[HIDCTL] cannot open the filesystem root");
      return;
    }
    while (true) {
      File f = dir.openNextFile();
      if (!f) break;
      String name = f.name();
      f.close();
      // Profiles sit in the LittleFS root and are named "<prefix>*.json".
      if (!name.startsWith(HIDCTL_PROFILE_PREFIX)) continue;
      if (!name.endsWith(HIDCTL_PROFILE_SUFFIX)) continue;
      if (profile_count >= HIDCTL_MAX_PROFILES) {
        USER_PRINTLN("[HIDCTL] too many profiles, ignoring the rest");
        break;
      }
      String path = String(HIDCTL_PROFILE_DIR) + name;
      File pf = WLED_FS.open(path, "r");
      if (!pf) continue;
      size_t sz = pf.size();
      if (sz == 0 || sz > 16384) { pf.close(); continue; }
      char* buf = (char*)heap_caps_calloc_prefer(sz + 1, 1, 2,
                                                MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL);
      if (!buf) { pf.close(); continue; }
      pf.read((uint8_t*)buf, sz);
      buf[sz] = '\0';
      pf.close();

      // ArduinoJson v6: a document needs a memory pool. The profile is a
      // few kB at most; give it room to spare.
      DynamicJsonDocument doc(sz + 2048);
      DeserializationError err = deserializeJson(doc, buf);
      heap_caps_free(buf);
      if (err) {
        USER_PRINTF("[HIDCTL] %s: bad JSON (%s)\n", path.c_str(), err.c_str());
        continue;
      }
      JsonObject j = doc.as<JsonObject>();
      if (parseProfile(j, profiles[profile_count])) {
        USER_PRINTF("[HIDCTL] profile %u: %s (0x%04X:0x%04X) %u controls, %u buttons%s\n",
                    profile_count, profiles[profile_count].name,
                    profiles[profile_count].vid, profiles[profile_count].pid,
                    profiles[profile_count].control_count,
                    profiles[profile_count].button_count,
                    profiles[profile_count].opendeck ? " opendeck" : "");
        profile_count++;
      } else {
        USER_PRINTF("[HIDCTL] %s: profile rejected\n", path.c_str());
      }
    }
    dir.close();
  }

  // -------------------------------------------------------------------------
  // device matching
  // -------------------------------------------------------------------------
 public:
  // Does any profile want this device? Returns the profile index, or 0xFF.
  uint8_t matchDevice(uint16_t vid, uint16_t pid) const {
    for (uint8_t i = 0; i < profile_count; i++) {
      if (profiles[i].vid == vid && profiles[i].pid == pid) return i;
    }
    return 0xFF;
  }

  const char* profileName() const {
    if (active_profile < profile_count) return profiles[active_profile].name;
    return "";
  }

  // Is this profile one of the OpenDeck-family decks? They are not plain
  // HID devices: their packets are a command protocol, and the way they
  // go on the wire is different too.
  bool profileIsOpendeck(uint8_t idx) const {
    return idx < profile_count && profiles[idx].opendeck;
  }

  void setActiveProfile(uint8_t idx) { active_profile = idx; }

  // The USB host client found a device. Bind the first profile that wants it.
  void onDeviceConnected(uint16_t vid, uint16_t pid) {
    // Re-read the profiles: one may have been edited while the controller
    // was unplugged, and this is the moment it comes back.
    loadProfiles();
    uint8_t idx = matchDevice(vid, pid);
    if (idx == 0xFF) {
      USER_PRINTF("[HIDCTL] device 0x%04X:0x%04X — no profile wants it\n", vid, pid);
      return;
    }
    active_profile = idx;
    resetControls();
    buttons = 0;
    // The wheel's position belongs to the connection: forget the old one so
    // the first report after a reconnect is not read as a turn.
    wheelSeen = false;
    wheelLast = 0;
    USER_PRINTF("[HIDCTL] %s connected\n", profiles[idx].name);
    // An OpenDeck-family deck needs its session opened: DIS resets the
    // display, LIG sets the backlight, and CONNECT keeps the session
    // alive. Its keys are LCDs, so they are given pictures. The work
    // itself is done by loop() - this is not the task that owns the USB.
    if (profiles[idx].opendeck) {
      // How the deck's packets go on the wire, from the capture of a
      // host that drives it and from the reference:
      //
      //   - the report-ID byte is not on the wire. The packet starts with
      //     "CRT"; a PC's HID stack strips the byte the reference writes,
      //     and the device drops any frame that does not start "CRT".
      //   - the report is 1024 bytes and it is sent as it is: no
      //     terminator. The zero-length packet in a Windows capture is
      //     the Windows HID stack flushing its report.
      hid_usb_set_out_drop_leading(true);
      hid_usb_set_out_shift(false);
      hid_usb_set_out_zlp(false);
      opendeck_pending_init = true;
      opendeck_pending_paint = true;
    }
#ifdef F1_PROBE
    // The discovery probe: it walks the candidate report shapes so the LED
    // protocol can be worked out against the hardware. Off by default - it
    // flashes the display and the pads on every connect.
    probeFormats();
#endif
    paint();
  }

  void onDeviceDisconnected() {
    if (active_profile != 0xFF) {
      USER_PRINTF("[HIDCTL] %s disconnected\n", profiles[active_profile].name);
    }
    active_profile = 0xFF;
    buttons = 0;
  }

  // -------------------------------------------------------------------------
  // action dispatch
  // -------------------------------------------------------------------------
  void runAction(const char* action, uint8_t value) {
    if (!action || !*action) return;

    // ---- preset slots ------------------------------------------------------
    if (!strncmp(action, "savepreset", 10)) {
      int slot = atoi(action + 10);
      if (slot < 1 || slot > 250) return;
      char nm[40];
      snprintf(nm, sizeof(nm), "HIDCTL preset %d", slot);
      savePreset((uint8_t)slot, nm);
      stateUpdated(CALL_MODE_BUTTON_PRESET);
      return;
    }
    if (!strncmp(action, "preset", 6)) {
      int slot = atoi(action + 6);
      // There are 250 presets and 16 pages of 16 slots, so the slot a
      // page addresses can be well past 64.
      if (slot < 1 || slot > 250) return;
      // Picking a preset stops the playlist that was running - the same
      // "switch to a preset cleanly" pattern the MIDI and ProLink usermods
      // use. Without it the playlist would carry on over the top of the
      // preset the user just picked.
      if (currentPlaylist >= 0) unloadPlaylist();
      // The core's PresetApplied event sets the tracked preset and the
      // page; if the preset is not there, neither moves.
      applyPreset((uint8_t)slot, CALL_MODE_BUTTON_PRESET);
      handlePresets();
      return;
    }

    // ---- the tempo tracker -------------------------------------------------
    if (!strcmp(action, "downbeat")) {
      // "The beat sounding now is beat one." The same request the serial
      // console's 'j' makes.
#ifdef USERMOD_AUDIOREACTIVE
      audioReactiveTempoCommand('j');
#endif
      return;
    }
    if (!strcmp(action, "cleardownbeat")) {
#ifdef USERMOD_AUDIOREACTIVE
      audioReactiveTempoCommand('J');
#endif
      return;
    }
    if (!strcmp(action, "relock")) {
#ifdef USERMOD_AUDIOREACTIVE
      audioReactiveTempoCommand('q');
#endif
      return;
    }
    if (!strcmp(action, "showpage")) {
      // Show the page on the display for a moment.
      showPageBriefly();
      return;
    }

    // ---- parameters --------------------------------------------------------
    if (!strcmp(action, "bri")) {
      bri = value;
      strip.setBrightness(bri, true);
      stateUpdated(CALL_MODE_BUTTON);
      return;
    }
    if (!strcmp(action, "effectSpeed")) {
      effectSpeed = value;
      applyValuesToSelectedSegs();
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }
    if (!strcmp(action, "effectIntensity")) {
      effectIntensity = value;
      applyValuesToSelectedSegs();
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }
    if (!strcmp(action, "effectPalette")) {
      uint16_t pc = strip.getPaletteCount();
      if (pc == 0) return;
      uint8_t p = (uint8_t)((uint16_t)value * pc / 256);
      if (p >= pc) p = pc - 1;
      effectPalette = p;
      applyValuesToSelectedSegs();
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }
    if (!strcmp(action, "effectCustom1")) {
      strip.getMainSegment().custom1 = value;
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }
    if (!strcmp(action, "effectCustom2")) {
      strip.getMainSegment().custom2 = value;
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }
    // per-segment brightness: seg0bri .. seg7bri
    // (this fork keeps the per-segment level in Segment::opacity — the
    //  segment's "bri" in the JSON API maps to the same field)
    if (!strncmp(action, "seg", 3) && isdigit(action[3])) {
      uint8_t seg = (uint8_t)(action[3] - '0');
      if (!strcmp(action + 4, "bri")) {
        if (seg < strip.getSegmentsNum()) {
          strip.getSegment(seg).opacity = value;
          stateUpdated(CALL_MODE_BUTTON);
        }
        return;
      }
    }

    // ---- verbs -------------------------------------------------------------
    if (!strcmp(action, "power")) {
      toggleOnOff();
      stateUpdated(CALL_MODE_BUTTON);
      return;
    }
    if (!strcmp(action, "blackout")) {
      // A LATCH: press once and the room goes dark, press again and it
      // comes back at the brightness it had.
      if (blackout_latched) {
        blackout_latched = false;
        bri = bri_before_blackout;
        strip.setBrightness(bri, true);
      } else {
        blackout_latched = true;
        bri_before_blackout = bri;
        bri = 0;
        strip.setBrightness(0, true);
      }
      stateUpdated(CALL_MODE_BUTTON);
      return;
    }
    if (!strcmp(action, "ctrlbri")) {
      // The controller's own button brightness - SHIFT + knob4. It has a
      // floor: the pads must never go completely dark, or there is no way
      // to see which one is the active preset.
      ctrl_bri = value;
      if (ctrl_bri < CTRL_BRI_MIN) ctrl_bri = CTRL_BRI_MIN;
      USER_PRINTF("[HIDCTL] controller brightness %u\n", ctrl_bri);
      // On a deck the controller's own brightness is the backlight, which
      // is the "LIG" command. The same floor applies.
      if (active_profile < profile_count && profiles && profiles[active_profile].opendeck) {
        Opendeck::lig(led_report, (uint8_t)((uint16_t)ctrl_bri * 100 / 255));
        opendeckSend(led_report);
      }
      paint();
      return;
    }
    if (!strcmp(action, "full")) {
      bri = 255;
      strip.setBrightness(255, true);
      stateUpdated(CALL_MODE_BUTTON);
      return;
    }
    if (!strcmp(action, "nextfx")) { act_nextfx(); return; }
    if (!strcmp(action, "prevfx")) { act_prevfx(); return; }
    if (!strcmp(action, "nextpal")) { act_nextpal(); return; }
    if (!strcmp(action, "prevpal")) { act_prevpal(); return; }
    if (!strcmp(action, "nextpreset")) { jumpPreset(+1); return; }
    if (!strcmp(action, "prevpreset")) { jumpPreset(-1); return; }
    if (!strcmp(action, "toggleFreeze")) {
      Segment& seg = strip.getMainSegment();
      seg.freeze = !seg.freeze;
      stateUpdated(CALL_MODE_BUTTON);
      return;
    }
    if (!strncmp(action, "seg", 3) && isdigit(action[3]) && action[4] == '\0') {
      uint8_t seg = (uint8_t)(action[3] - '0');
      if (seg < strip.getSegmentsNum()) {
        for (uint8_t i = 0; i < strip.getSegmentsNum(); i++) {
          strip.getSegment(i).selected = (i == seg);
        }
        stateUpdated(CALL_MODE_BUTTON);
        wsPushIfDue();
      }
      return;
    }
  }

  void act_nextfx() {
    const uint16_t count = getEffectDisplayCount();
    if (count > 0) {
      uint16_t cur_pos = 0;
      for (uint16_t i = 0; i < count; i++) {
        if (getEffectIdByDisplayIndex(i) == effectCurrent) { cur_pos = i; break; }
      }
      effectCurrent = getEffectIdByDisplayIndex((uint16_t)((cur_pos + 1) % count));
    } else {
      effectCurrent = (uint8_t)((effectCurrent + 1) % strip.getModeCount());
    }
    applyValuesToSelectedSegs();
    colorUpdated(CALL_MODE_BUTTON);
    stateUpdated(CALL_MODE_BUTTON);
    wsPushIfDue();
  }

  void act_prevfx() {
    const uint16_t count = getEffectDisplayCount();
    if (count > 0) {
      uint16_t cur_pos = 0;
      for (uint16_t i = 0; i < count; i++) {
        if (getEffectIdByDisplayIndex(i) == effectCurrent) { cur_pos = i; break; }
      }
      effectCurrent = getEffectIdByDisplayIndex((uint16_t)(cur_pos == 0 ? count - 1 : cur_pos - 1));
    } else {
      effectCurrent = (effectCurrent == 0)
          ? (uint8_t)(strip.getModeCount() - 1)
          : (uint8_t)(effectCurrent - 1);
    }
    applyValuesToSelectedSegs();
    colorUpdated(CALL_MODE_BUTTON);
    stateUpdated(CALL_MODE_BUTTON);
    wsPushIfDue();
  }

  void act_nextpal() {
    effectPalette = (uint8_t)((effectPalette + 1) % strip.getPaletteCount());
    applyValuesToSelectedSegs();
    colorUpdated(CALL_MODE_BUTTON);
    stateUpdated(CALL_MODE_BUTTON);
  }

  void act_prevpal() {
    effectPalette = (effectPalette == 0)
        ? (uint8_t)(strip.getPaletteCount() - 1)
        : (uint8_t)(effectPalette - 1);
    applyValuesToSelectedSegs();
    colorUpdated(CALL_MODE_BUTTON);
    stateUpdated(CALL_MODE_BUTTON);
  }

  // Jump to the next/previous preset, skipping empty slots and playlists.
  void jumpPreset(int direction) {
    // Start from the preset that is really active, not from the core's
    // currentPreset - that is 0 as soon as anything has changed.
    int slot = tracked_active_preset ? (int)tracked_active_preset : 1;
    int start = slot;
    for (int i = 0; i < 250; i++) {
      slot += direction;
      if (slot < 1)   slot = 250;
      if (slot > 250) slot = 1;
      if (slot == start) break;
      if (getCachedPresetExists(slot)
          && !(presetCache != nullptr && presetCache[slot].isPlaylist)) {
        break;
      }
    }
    if (getCachedPresetExists(slot)
        && !(presetCache != nullptr && presetCache[slot].isPlaylist)) {
      // Picking a preset stops the playlist: while it runs, the core
      // puts its own preset back on the next tick and the jump is
      // invisible.
      if (currentPlaylist >= 0) unloadPlaylist();
      applyPreset((uint8_t)slot, CALL_MODE_BUTTON_PRESET);
      handlePresets();
    }
  }

  void wsPushIfDue() {
    if (millis() - lastWsPush < WS_PUSH_INTERVAL_MS) return;
    lastWsPush = millis();
    interfaceUpdateCallMode = CALL_MODE_BUTTON;
    lastInterfaceUpdate = 0;
    updateInterfaces(CALL_MODE_BUTTON);
  }

  // -------------------------------------------------------------------------
  // The output report: LEDs and the display
  // -------------------------------------------------------------------------
  // The canonical seven-segment stroke order. The profile lists its own
  // order; a digit is a bitmask over this one.
  static const char* segmentName(uint8_t i) {
    static const char* names[HIDCTL_MAX_SEGMENTS] = {
      "dot", "middle", "lower-right", "upper-right",
      "top", "upper-left", "lower-left", "bottom"
    };
    return (i < HIDCTL_MAX_SEGMENTS) ? names[i] : "";
  }

  static uint8_t digitMask(uint8_t digit) {
    // bit0 dot, bit1 middle, bit2 lower-right, bit3 upper-right,
    // bit4 top, bit5 upper-left, bit6 lower-left, bit7 bottom
    static const uint8_t masks[10] = {
      0xFC, 0x0C, 0xDA, 0x9E, 0x2E, 0xB6, 0xF6, 0x1C, 0xFE, 0xBE
    };
    return (digit < 10) ? masks[digit] : 0;
  }

  static int segmentIndexByName(const char* name) {
    for (int i = 0; i < HIDCTL_MAX_SEGMENTS; i++) {
      if (!strcmp(name, segmentName(i))) return i;
    }
    return -1;
  }

  // Is a button with this name currently held?
  bool buttonHeld(const HidctlProfile& p, const char* name) {
    for (uint8_t i = 0; i < p.button_count; i++) {
      if (!strcmp(p.buttons[i].name, name)) {
        return (buttons & (1UL << p.buttons[i].index)) != 0;
      }
    }
    return false;
  }

  // Which segment is selected (the first one that is).
  uint8_t selectedSegment() {
    for (uint8_t i = 0; i < strip.getSegmentsNum(); i++) {
      if (strip.getSegment(i).selected) return i;
    }
    return 0;
  }

  // Try every plausible shape of the output report, lighting everything,
  // and log which one the device accepts. Used to pin down a controller's
  // LED protocol when the sources disagree (report type, data length).

  // Paint the LEDs and the display, then send the output report.
  //
  // The report goes out on the interface's interrupt OUT endpoint as the
  // whole report: the ID byte first, then the data. The device does not
  // answer HID class requests (SET_REPORT stalls on every report ID and
  // length), so the OUT endpoint is the only way in.
  // -------------------------------------------------------------------------
  // The LED demo
  // -------------------------------------------------------------------------
  // A triangle wave over 256 steps: 0, 0x7F, 0, ...
  static uint8_t triangle(uint8_t phase) {
    uint8_t p = phase & 0xFF;
    return (p < 128) ? (uint8_t)(p * 2) : (uint8_t)((255 - p) * 2);
  }

  // A full-saturation hue on the wheel, 0..255.
  static void hue2rgb(uint8_t hue, uint8_t& r, uint8_t& g, uint8_t& b) {
    uint8_t region = hue / 43;
    uint8_t rem = (uint8_t)((hue - region * 43) * 6);
    uint8_t rising = (uint8_t)(rem * 21);           // 0..127
    uint8_t falling = (uint8_t)(127 - rising);
    switch (region) {
      case 0: r = 0x7F; g = rising;  b = 0;      break;
      case 1: r = falling; g = 0x7F;  b = 0;      break;
      case 2: r = 0;      g = 0x7F;  b = rising;  break;
      case 3: r = 0;      g = falling; b = 0x7F; break;
      case 4: r = rising;  g = 0;      b = 0x7F; break;
      default: r = 0x7F; g = 0;      b = falling; break;
    }
  }

  // The first few bytes of a report, as hex.
  static const char* bytes_to_hex(const uint8_t* d, uint8_t n) {
    static char buf[40];
    int len = 0;
    buf[0] = '\0';
    for (uint8_t i = 0; i < n && len < (int)sizeof(buf) - 4; i++) {
      len += snprintf(buf + len, sizeof(buf) - len, "%02X ", d[i]);
    }
    return buf;
  }

  // Write a number on the seven-segment display.
  void showNumber(uint8_t* report, HidctlProfile& p, uint16_t value) {
    if (!p.output.display.present) return;
    uint16_t divisor = 1;
    for (uint8_t k = 1; k < p.output.display.digit_count; k++) divisor *= 10;
    for (uint8_t d = 0; d < p.output.display.digit_count; d++) {
      uint8_t digit = (uint8_t)((value / divisor) % 10);
      divisor = (divisor > 1) ? divisor / 10 : 1;
      uint8_t mask = digitMask(digit);
      uint16_t base = p.output.display.digit_offset[d];
      for (uint8_t s = 0; s < p.output.display.segments_per_digit; s++) {
        int canon = segmentIndexByName(p.output.display.segment_names[s]);
        bool on = (canon >= 0) && ((mask >> canon) & 1);
        report[base + s] = on ? 0x7F : 0x00;
      }
    }
  }

  // Write a letter on the left-hand digit ("P" for the page).
  void showLetter(uint8_t* report, HidctlProfile& p, char letter) {
    if (!p.output.display.present || p.output.display.digit_count < 2) return;
    // The strokes of a "P": top, upper-right, upper-left, middle - and the
    // lower-left bar as well, so the stem runs the full height. Without it
    // the letter is a bowl with a floating middle bar and reads as a
    // square. The segment order is the device's own: DP, G(middle),
    // C(lower-right), B(upper-right), A(top), F(upper-left), E(lower-left),
    // D(bottom) - so B is the upper-right bar and E the lower-left one.
    uint8_t mask = 0;
    switch (letter) {
      case 'P': mask = (1u << 4) | (1u << 3) | (1u << 5) | (1u << 6) | (1u << 1); break;
      case 'E': mask = (1u << 4) | (1u << 5) | (1u << 1) | (1u << 6) | (1u << 7); break;
      default:  mask = 0; break;
    }
    uint16_t base = p.output.display.digit_offset[0];
    for (uint8_t s = 0; s < p.output.display.segments_per_digit; s++) {
      int canon = segmentIndexByName(p.output.display.segment_names[s]);
      bool on = (canon >= 0) && ((mask >> canon) & 1);
      report[base + s] = on ? 0x7F : 0x00;
    }
  }

  // The demo picture: the transport buttons chase, the lower buttons sweep,
  // the pads turn a rainbow and the display counts.
  void paintDemo(uint8_t* report, HidctlProfile& p) {
    uint32_t t = millis() - demoStart;

    // The transport row: a chase, each button a step behind the last.
    for (uint8_t i = 0; i < 8; i++) {
      report[17 + i] = triangle((uint8_t)((t / 50) + i * 10));
    }
    // The lower buttons: a sweep across both halves of each button.
    for (uint8_t i = 0; i < 4; i++) {
      for (uint8_t s = 0; s < 2; s++) {
        report[73 + i * 2 + s] = triangle((uint8_t)((t / 40) + i * 12 + s * 6));
      }
    }
    // The pads: a rainbow that turns, and breathes.
    uint8_t level = triangle((uint8_t)(t / 100));
    for (uint8_t k = 0; k < 16; k++) {
      uint8_t r, g, b;
      hue2rgb((uint8_t)(k * 16 + t / 10), r, g, b);
      r = (uint8_t)((uint16_t)r * level / 0x7F);
      g = (uint8_t)((uint16_t)g * level / 0x7F);
      b = (uint8_t)((uint16_t)b * level / 0x7F);
      uint16_t base = 25 + k * 3;
      report[base + 0] = b;
      report[base + 1] = r;
      report[base + 2] = g;
    }
    // The display: a counter.
    showNumber(report, p, (uint16_t)((t / 500) % 100));
  }

  // Switch the demo on or off.
  void setDemo(bool on) {
    if (on == demoActive) return;
    demoActive = on;
    demoStart = millis();
    USER_PRINTF("[HIDCTL] LED demo %s\n", on ? "on" : "off");
  }

  // The serial console's key.
  void toggleDemo() { setDemo(!demoActive); }

  void paint() {
    if (!profiles || active_profile >= profile_count) return;
    HidctlProfile& p = profiles[active_profile];
    if (!p.output.present) return;
    if (!paint_enabled) return;

    // WLED fires a state change often; the LEDs do not need to follow every
    // one of them. A blink is the exception - it has to be repainted fast
    // enough to show the rate it was asked for.
    static uint32_t lastPaint = 0;
    uint32_t minGap = blinkActive() ? 20 : 100;
    if (millis() - lastPaint < minGap) return;
    lastPaint = millis();

    uint8_t report[HIDCTL_MAX_OUTPUT];
    memset(report, 0, sizeof(report));
    report[0] = p.output.report_id;

    // The page follows the preset, but only when a preset is really
    // applied - the core's PresetApplied event sets it. A page that is
    // being browsed (nothing saved on it) stays where it was put.

    // The beat that is sounding now, from the tempo tracker.
    {
      ArTempoFrame tf;
      currentBeat = 0;
      if (arCopyTempo(&tf) && tf.beatInBar >= 1 && tf.beatInBar <= 4) {
        currentBeat = tf.beatInBar;
      }
    }
    music_slot = queryAutoPlaylistMusicSlot();

    // --- the display: the page for a moment, then the preset number ---
    uint16_t val = (tracked_active_preset >= 1 && tracked_active_preset <= 99)
                   ? tracked_active_preset : 0;
    if (millis() < showPageUntil) {
      // The encoder turned the page: show the page. That is what the
      // encoder is for, so it is what the display shows.
      if (page < 10) {
        // "P" and the page's number on the right-hand digit.
        showLetter(report, p, 'P');
        if (p.output.display.present && p.output.display.digit_count >= 2) {
          uint16_t base = p.output.display.digit_offset[1];
          uint8_t mask = digitMask(page);
          for (uint8_t s = 0; s < p.output.display.segments_per_digit; s++) {
            int canon = segmentIndexByName(p.output.display.segment_names[s]);
            bool on = (canon >= 0) && ((mask >> canon) & 1);
            report[base + s] = on ? 0x7F : 0x00;
          }
        }
      } else {
        // Ten and over: there is no room for the letter as well, so the
        // two digits are the page number itself. There is no decimal
        // point to mark it with - the F1's profile does not set one.
        showNumber(report, p, page);
      }
    } else if (millis() < showWheelUntil) {
      // The encoder moved but the page did not (it is at the first or the
      // last one): show the encoder's own byte. This is the one thing the
      // display can do that no console and no web page can.
      showNumber(report, p, wheelLast);
    } else {
      showNumber(report, p, val);
    }

    // The display blinks as a whole when it is told to.
    if (blinkPhase("") == 0) {
      for (uint8_t d = 0; d < p.output.display.digit_count; d++) {
        uint16_t base = p.output.display.digit_offset[d];
        for (uint8_t s = 0; s < p.output.display.segments_per_digit; s++) {
          report[base + s] = 0x00;
        }
      }
    }

    // The playlist chaser, like the MIDI usermod's: while a playlist is
    // running, the parent's pad pulses slowly and the child that is playing
    // blinks fast. The blinks are set before the lights are painted, so the
    // phase they are on is the one the lights get.
    const bool playlist_active = (currentPlaylist >= 0);
    const int playlist_parent = playlist_active ? (int)currentPlaylist : 0;
    // Say it once when the chaser's targets change, so the console shows
    // which pad is the parent and which is the child.
    {
      static int last_parent = -1;
      static int last_child = -1;
      if (playlist_parent != last_parent ||
          (int)tracked_active_preset != last_child) {
        last_parent = playlist_parent;
        last_child = (int)tracked_active_preset;
        USER_PRINTF("[HIDCTL] playlist %d, child %d\n", last_parent, last_child);
      }
    }
    if (playlist_active) {
      for (uint8_t i = 0; i < p.output.light_count; i++) {
        const char* nm = p.output.lights[i].name;
        if (strncmp(nm, "pad", 3)) continue;
        int slot = atoi(nm + 3) + (int)pageOffset();
        if (slot < 1 || slot > 250) continue;
        if (slot == playlist_parent) {
          blinkSet(nm, 1.0f, 1000, true);          // the parent: a slow pulse
        } else if (slot == (int)tracked_active_preset) {
          blinkSet(nm, 4.0f, 1000, true);          // the child: a fast blink
        }
      }
    }

    if (demoActive) {
      paintDemo(report, p);
      sendLedReport(p, report);
      return;
    }

    // --- the lights ---
    for (uint8_t i = 0; i < p.output.light_count; i++) {
      const HidctlLight& li = p.output.lights[i];
      uint8_t b = 0, r = 0, g = 0;   // the colour bytes are blue, red, green
      const char* nm = li.name;

      if (!strncmp(nm, "pad", 3)) {
        // The pad shows the state of its preset, in the same categories the
        // APC Mini's pads use:
        //   the active preset          green
        //   a saved preset             blue
        //   a saved playlist           magenta (red when it is a one-shot)
        //   the music playlist         yellow
        // The pads address the current page, so the colours do too.
        int slot = atoi(nm + 3) + (int)pageOffset();
        if (slot >= 1 && slot <= 250) {
          const bool is_music = (music_slot > 0 && slot == (int)music_slot);
          const bool is_playlist = presetIsPlaylist((uint8_t)slot);
          if (is_music) {
            r = g = 0x7F;                    // yellow
          } else if (is_playlist) {
            // A finite playlist is a one-shot: red. One that loops is
            // magenta. The MIDI usermod paints its pads the same way.
            const bool finite = getPresetRepeat((uint8_t)slot) > 0;
            if (finite) { r = 0x7F; }          // red
            else        { r = b = 0x7F; }      // magenta
          } else if (slot == (int)tracked_active_preset && getCachedPresetExists(slot)) {
            g = 0x7F;                          // green
          } else if (getCachedPresetExists(slot)) {
            b = 0x7F;                          // blue
          }
        }
      } else if (!strncmp(nm, "bar", 3)) {
        // The four lower buttons are the BEAT COUNTER: the one that is
        // sounding is lit.
        int n = atoi(nm + 3);
        if (n >= 1 && n <= 4 && n == currentBeat) {
          r = g = b = 0x7F;
        }
      } else if (buttonHeld(p, nm)) {
        // Any other button lights while it is held.
        r = g = b = 0x7F;
      }

      // A blinking light: white in its "on" half (a flash), or its own
      // colour with the off half dark (the playlist chaser).
      const int phase = blinkPhase(nm);
      if (phase == 0) {
        b = r = g = 0;
      } else if (phase == 1 && !blinkKeepsColour(nm)) {
        r = g = b = 0x7F;
      }

      // The controller's own brightness (SHIFT + knob4). The display is
      // left alone - it is the readout.
      if (ctrl_bri < 255) {
        b = (uint8_t)((uint16_t)b * ctrl_bri / 255u);
        r = (uint8_t)((uint16_t)r * ctrl_bri / 255u);
        g = (uint8_t)((uint16_t)g * ctrl_bri / 255u);
      }

      if (li.bytes == 3) {
        report[li.offset + 0] = b;
        report[li.offset + 1] = r;
        report[li.offset + 2] = g;
      } else if (li.bytes == 2) {
        report[li.offset + 0] = (r || g || b) ? 0x7F : 0x00;
        report[li.offset + 1] = report[li.offset + 0];
      } else {
        report[li.offset] = (r || g || b) ? 0x7F : 0x00;
      }
    }

    sendLedReport(p, report);
  }

  // Send the LED report to the device.
  //
  // The report is 81 bytes - the report ID and 80 brightness values - and
  // the F1's interrupt OUT endpoint takes only 64 bytes at a time. So it
  // does not go there: it goes over the control pipe as a SET_REPORT,
  // which is what a report that long needs. A host that reads the encoder
  // (the capture in usb4.pcapng) writes nothing to the OUT endpoint at
  // all.
  //
  // "ledout" puts it back on the OUT endpoint:
  //   POST /json/state  {"HIDCTL":{"ledout":true}}
  // Queue the LED report. It is BUILT here and SENT by loop(): the USB
  // host stack belongs to the main loop's task, and this can be called
  // from an event handler (the publisher's task), from the core's state
  // change callback, or from the web API.
  void sendLedReport(const HidctlProfile& p, const uint8_t* report) {
    uint16_t len = p.output.length;
    if (len > HIDCTL_MAX_OUTPUT) len = HIDCTL_MAX_OUTPUT;
    memcpy(led_report, report, len);
    led_report_len = len;
    led_report_pending = true;
  }

  // Send the queued LED report. Only the main loop calls this.
  void flushLedReport() {
    if (!led_report_pending) return;
    led_report_pending = false;
    if (!profiles || active_profile >= profile_count) return;
    HidctlProfile& p = profiles[active_profile];
    if (led_out_endpoint) {
      hid_usb_send_out_endpoint(led_report, led_report_len);
      return;
    }
    // The report ID goes in wValue, not in the data.
    hid_usb_send_output_report(p.output.report_id, led_report + 1,
                                (uint16_t)(led_report_len - 1));
  }

  // -------------------------------------------------------------------------
  // incoming report
  // -------------------------------------------------------------------------
 public:
  // Apply a control value that was held back by the throttle. This is
  // called from the report handler and from loop(), so the last position
  // of a sweep is applied even if the controller stops reporting when it
  // is left alone.
  void flushPendingControls() {
    if (!profiles || active_profile >= profile_count) return;
    HidctlProfile& p = profiles[active_profile];
    const uint32_t now = millis();
    for (uint8_t i = 0; i < p.control_count; i++) {
      if (!control_pending[i]) continue;
      if (control_has_applied[i] &&
          (now - control_applied_ms[i]) < CONTROL_APPLY_INTERVAL_MS) continue;
      applyControl(p, i, control_value[i], now);
      control_pending[i] = false;
    }
  }

  // Drive one control into WLED. The soft takeover, the "did the result
  // actually change" test and the throttle all happen here.
  void applyControl(HidctlProfile& p, uint8_t idx, uint16_t raw, uint32_t now) {
    HidctlControl& ctl = p.controls[idx];
    uint32_t full = (ctl.bits >= 16) ? 0xFFFFu : ((1u << ctl.bits) - 1);
    uint8_t value = (uint8_t)((uint32_t)raw * 255u / full);

    // A modifier turns the control into something else: SHIFT + knob4 is
    // the controller's own brightness, not the palette.
    const char* act = ctl.action;
    if (ctl.mod_action[0] && (buttons & modifier_mask) != 0) act = ctl.mod_action;
    if (!act[0]) return;

    // Soft takeover: the control has to cross the parameter's current
    // value before it starts driving it, so a knob that sits somewhere
    // else does not yank the value when it is first touched.
    if (!control_taken[idx]) {
      int cur = currentValueOf(act);
      if (cur >= 0) {
        int curRaw = (int)((uint32_t)cur * full / 255u);
        int delta = (int)raw - curRaw;
        if (delta < 0) delta = -delta;
        int wiggle = (int)(full / 255u * 8u) + 1;   // ~8/255 of the range
        if (delta > wiggle) {
          // Say it once, not once a report.
          if (!control_waited[idx]) {
            USER_PRINTF("[HIDCTL] %s: waiting for takeover (raw %u, target %u)\n",
                        ctl.name, raw, (unsigned)cur);
            control_waited[idx] = true;
          }
          return;    // not driven: the baseline stays where it was
        }
      }
      control_taken[idx] = 1;
      control_waited[idx] = false;
    }

    // The result did not change, so there is nothing to drive. The baseline
    // still follows the knob, so its resting point is known.
    if (control_has_applied[idx] && value == control_mapped[idx]) {
      control_applied[idx] = raw;
      control_applied_ms[idx] = now;
      return;
    }
    control_mapped[idx] = value;
    control_applied[idx] = raw;
    control_applied_ms[idx] = now;
    control_has_applied[idx] = true;

    if (!act[0]) return;
    if (now - control_log_ms[idx] >= CONTROL_LOG_INTERVAL_MS) {
      control_log_ms[idx] = now;
      USER_PRINTF("[HIDCTL] %s = %u -> %s = %u\n", ctl.name, raw, act, value);
    }
    runAction(act, value);
  }

  // -------------------------------------------------------------------------
  // an OpenDeck-family deck
  // -------------------------------------------------------------------------
  //
  // Its input is one event per report, with the event in a message ID, and
  // its output is the ASCII "CRT" command protocol.
  //
  // The report is the raw USB packet, so it still carries the report ID
  // (0x00) in front. The reference reads it through hidraw, which strips
  // that byte, so it finds "ACK" at byte 0; here it is at byte 1. Look for
  // it either way.
  void handleOpendeckReport(HidctlProfile& p, const uint8_t* data, uint16_t len) {
    int ack = -1;
    if (len >= 12 && data[0] == 'A' && data[1] == 'C' && data[2] == 'K') ack = 0;
    else if (len >= 13 && data[1] == 'A' && data[2] == 'C' && data[3] == 'K') ack = 1;
    if (ack < 0) return;                    // not one of the device's own

    const uint8_t msg   = data[ack + 9];
    const uint8_t state = data[ack + 10];
    const Opendeck::EventInfo ev = Opendeck::decodeMessage(msg);
    USER_PRINTF("[HIDCTL] deck: event %u, message 0x%02X, state %u\n",
                (unsigned)len, (unsigned)msg, (unsigned)state);
    if (ev.type == Opendeck::Event::None) return;

    // The device reports a press and a release as two reports, so the
    // action runs on the rising edge. The touch strip's zones report only
    // one state, and they act on every report.
    if (ev.type == Opendeck::Event::ButtonDown ||
        ev.type == Opendeck::Event::EncoderDown) {
      const bool plain = Opendeck::isPlainButtonMessage(msg);
      if (!plain && state == 0) return;        // the release
    }

    switch (ev.type) {
      case Opendeck::Event::ButtonDown: {
        // The button index is the one the reference uses, so the profile's
        // button_map lines up with it.
        const uint8_t idx = ev.index;
        if (idx >= 32) break;
        const char* act = buttonActionFor(p, idx);
        if (act && act[0]) {
          // The page's offset applies here too: the keys on a page are
          // the slots from pageOffset()+1 up, or the deck's pictures and
          // its actions would disagree.
          char shifted[HIDCTL_MAX_ACTION + 8];
          const char* use = withPageOffset(act, shifted, sizeof(shifted));
          USER_PRINTF("[HIDCTL] %s -> %s\n", buttonNameFor(p, idx), use);
          runAction(use, 255);
        }
        paint();
        break;
      }
      case Opendeck::Event::EncoderDown: {
        if (ev.index >= (p.od_encoders ? p.od_encoders : 4)) break;
        HidctlEncoder& enc = p.encoders[ev.index];
        if (enc.push_action[0]) {
          USER_PRINTF("[HIDCTL] %s push -> %s\n", enc.name, enc.push_action);
          runAction(enc.push_action, 255);
        }
        paint();
        break;
      }
      case Opendeck::Event::EncoderTwist: {
        if (ev.index >= (p.od_encoders ? p.od_encoders : 4)) break;
        HidctlEncoder& enc = p.encoders[ev.index];
        if (!enc.action[0]) break;
        if (!strcmp(enc.action, "page")) {
          // The encoder browses the pages of slots. It must not change
          // the preset itself - that is the keys' job.
          turnPage(ev.delta);
          break;
        }
        // A twist is plus or minus one, so the value it drives is the one
        // it drove last, moved by one.
        int v = (int)enc.value + ev.delta;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        enc.value = (uint8_t)v;
        USER_PRINTF("[HIDCTL] %s twist -> %s = %u\n", enc.name, enc.action, v);
        runAction(enc.action, v);
        break;
      }
      default:
        break;
    }
    (void)state;
  }

  // The action with the page's offset applied: the presets on a page are
  // the slots from pageOffset()+1 up. The buffer is the caller's.
  const char* withPageOffset(const char* act, char* buf, size_t buflen) {
    if (!strncmp(act, "preset", 6) && isdigit(act[6])) {
      uint16_t slot = (uint16_t)(atoi(act + 6) + pageOffset());
      snprintf(buf, buflen, "preset%u", slot);
      return buf;
    }
    return act;
  }

  // The profile's button with that index, or null.
  const HidctlButton* buttonByIndex(uint8_t index) {
    if (active_profile >= profile_count || !profiles) return nullptr;
    HidctlProfile& p = profiles[active_profile];
    for (uint8_t i = 0; i < p.button_count; i++) {
      if (p.buttons[i].index == index) return &p.buttons[i];
    }
    return nullptr;
  }

  // The action a button index drives, and its name, from the profile.
  const char* buttonActionFor(HidctlProfile& p, uint8_t index) {    for (uint8_t i = 0; i < p.button_count; i++) {
      if (p.buttons[i].index == index) return p.buttons[i].action;
    }
    return nullptr;
  }
  const char* buttonNameFor(HidctlProfile& p, uint8_t index) {
    for (uint8_t i = 0; i < p.button_count; i++) {
      if (p.buttons[i].index == index) return p.buttons[i].name;
    }
    return "button";
  }

  // ---- the output side --------------------------------------------------
  //
  // Every command is zero-padded to 1025 bytes and the device ignores a
  // short one. The session has to be kept alive with CONNECT every ten
  // seconds, or the deck drops it and goes back to its own splash screen.
  //
  // The OUT pipe takes ONE transfer at a time, so the commands are queued
  // and go out one per pass of the main loop. A burst would have all but
  // the first refused.
  static constexpr uint8_t OPENDECK_CMD_MAX = 8;
  uint8_t opendeck_cmd[OPENDECK_CMD_MAX][16] = {};
  uint8_t opendeck_cmd_len[OPENDECK_CMD_MAX] = {};
  uint8_t opendeck_cmd_count = 0;
  uint8_t opendeck_cmd_pos = 0;

  // Queue one command. It is small - every opcode is at most thirteen
  // bytes - and it is padded to the wire length when it goes out.
  void opendeckQueue(const uint8_t* body, size_t len) {
    if (opendeck_cmd_count >= OPENDECK_CMD_MAX) return;
    if (len > 16) len = 16;
    memcpy(opendeck_cmd[opendeck_cmd_count], body, len);
    opendeck_cmd_len[opendeck_cmd_count] = (uint8_t)len;
    opendeck_cmd_count++;
  }

  // Send the next queued command, if the pipe is free.
  bool opendeckFlushCmd() {
    if (opendeck_cmd_pos >= opendeck_cmd_count) return false;
    memset(led_report, 0, Opendeck::WIRE_LEN);
    memcpy(led_report, opendeck_cmd[opendeck_cmd_pos], opendeck_cmd_len[opendeck_cmd_pos]);
    if (!opendeckSend(led_report)) return false;
    opendeck_cmd_pos++;
    if (opendeck_cmd_pos >= opendeck_cmd_count) {
      opendeck_cmd_count = 0;
      opendeck_cmd_pos = 0;
    }
    return true;
  }

  bool opendeckCmdPending() { return opendeck_cmd_pos < opendeck_cmd_count; }

  void opendeckInit() {
    // The builders put the command at the front of the buffer and pad it;
    // only the first bytes matter here, so that is what is queued.
    Opendeck::dis(led_report);
    opendeckQueue(led_report, 9);
    Opendeck::lig(led_report, 50);
    opendeckQueue(led_report, 12);
    Opendeck::cle(led_report, Opendeck::CLEAR_ALL);
    opendeckQueue(led_report, 13);
    Opendeck::stp(led_report);
    opendeckQueue(led_report, 9);
    opendeck_last_keepalive = millis();
  }

  void opendeckKeepalive() {
    if (millis() - opendeck_last_keepalive < 9000) return;
    opendeck_last_keepalive = millis();
    Opendeck::connect(led_report);
    opendeckQueue(led_report, 13);
  }

  uint32_t opendeck_last_keepalive = 0;

  // Push the deck's pictures again, without opening a new session: the
  // reference sends its DIS once per connection and the device does not
  // like it repeated.
  void repaintDeck() {
    if (active_profile < profile_count && profiles && profiles[active_profile].opendeck) {
      opendeck_pending_paint = true;
    }
  }
  // The deck's USB work belongs to the main loop, like everything else:
  // the event bus runs its handlers in the PUBLISHER's task, and submitting
  // a transfer from there is how the USB stack got corrupted before.
  bool opendeck_pending_init = false;
  bool opendeck_pending_paint = false;

  // ---- the images -------------------------------------------------------
  //
  // The deck's keys are LCDs, so what it shows is a picture and not a
  // colour. A key image is 112x112 JPEG, rotated 180 degrees, and it is
  // pushed in 1024-byte chunks after a "BAT" header that names the key.
  //
  // The P4 has a hardware JPEG encoder, so the chip does the work. Its
  // input is in BGR order, and the buffers have to be in PSRAM: the
  // internal heap is nearly full on this board and a picture does not fit
  // in it. The DMA reads the picture, so the CPU's cache has to be told
  // about it - the DMA does not see it.
  static uint8_t* opendeckAlloc(size_t size) {
    const size_t aligned = (size + 63) & ~(size_t)63;
    uint8_t* p = (uint8_t*)heap_caps_aligned_alloc(64, aligned,
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!p) p = (uint8_t*)heap_caps_aligned_alloc(64, aligned,
                          MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    return p;
  }

#if HIDCTL_HAS_JPEG
  // The encoder's engine. It is made once and kept: the screenshot code
  // does the same, because a per-picture engine would pay for its working
  // memory every time.
  jpeg_encoder_handle_t opendeckEncoder = nullptr;

  bool opendeckEncoderGet(jpeg_encoder_handle_t& enc) {
    if (opendeckEncoder) { enc = opendeckEncoder; return true; }
    jpeg_encode_engine_cfg_t cfg = {};
    cfg.intr_priority = 0;        // the driver picks
    cfg.timeout_ms  = 1000;
    if (jpeg_new_encoder_engine(&cfg, &opendeckEncoder) != ESP_OK || !opendeckEncoder) {
      opendeckEncoder = nullptr;
      USER_PRINTLN("[HIDCTL] the JPEG encoder would not start");
      return false;
    }
    enc = opendeckEncoder;
    return true;
  }

  // Encode one picture for a key. `bgr` is the picture in the order the
  // encoder reads it (blue first). The JPEG comes back in `jpeg`.
  bool opendeckEncodeJpeg(const uint8_t* bgr, uint16_t w, uint16_t h,
                           uint8_t* jpeg, size_t jpeg_cap, size_t& jpeg_len) {
    jpeg_len = 0;
    jpeg_encoder_handle_t enc = nullptr;
    if (!opendeckEncoderGet(enc)) return false;

    jpeg_encode_cfg_t cfg = {};
    cfg.width         = w;
    cfg.height        = h;
    cfg.src_type      = JPEG_ENCODE_IN_FORMAT_RGB888;
    cfg.sub_sample    = JPEG_DOWN_SAMPLING_YUV444;
    cfg.image_quality = 90;

    // The CPU filled the picture, the DMA reads it.
    esp_cache_msync((void*)(uintptr_t)bgr, (size_t)w * h * 3,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);

    uint32_t outSize = 0;
    const esp_err_t err = jpeg_encoder_process(enc, &cfg, bgr,
                                               (uint32_t)((size_t)w * h * 3),
                                               jpeg, (uint32_t)jpeg_cap, &outSize);
    if (err != ESP_OK || outSize == 0) {
      USER_PRINTF("[HIDCTL] the JPEG encoder refused the picture (%d)\n", (int)err);
      return false;
    }
    // The DMA wrote the JPEG, the CPU reads it. The length is the size of
    // the JPEG, which is not a whole number of cache lines, so the sync is
    // told it need not be aligned.
    esp_cache_msync((void*)(uintptr_t)jpeg, outSize,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE
                    | ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    jpeg_len = outSize;
    return true;
  }

  // Which BAT slot a key's image goes to. The deck's own key numbering is
  // not the physical order - the reference remaps it, and the BAT target
  // is the remapped number plus one.
  static uint8_t opendeckSlotFor(uint8_t button_index) {
    uint8_t pos = Opendeck::mapPosition(button_index, false);
    if (pos == 0xFF) return 0xFF;
    return (uint8_t)(pos + 1);
  }

#endif  // HIDCTL_HAS_JPEG

  // Send one packet on the OUT endpoint. It is false when the pipe is
  // busy - the OUT pipe takes one transfer at a time - and the caller
  // tries again on the next pass.
  //
  // The gap is how long there has to be between two packets. A PC's HID
  // stack leaves 20 ms between them, and the deck's own firmware may
  // need the space: it answers each command on its IN endpoint, and a
  // command that arrives before the answer to the last one may be lost.
  uint32_t opendeck_out_gap = 0;         // ms, 0 = as fast as the pipe allows
  uint32_t opendeck_last_send_ms = 0;

  bool opendeckSend(uint8_t* built) {
    if (opendeck_out_gap &&
        (uint32_t)(millis() - opendeck_last_send_ms) < opendeck_out_gap) {
      return false;                       // not yet: try again next pass
    }
    bool ok = hid_usb_send_out_endpoint(built, Opendeck::WIRE_LEN);
    if (ok) opendeck_last_send_ms = millis();
    return ok;
  }

  // ---- the pictures -----------------------------------------------------
  //
  // A key's picture is a flat colour in the same categories the F1's pads
  // use: green for the preset that is running, blue for one that is saved,
  // red or magenta for a playlist, yellow for the music playlist. The
  // picture is drawn in the encoder's order (blue first) and rotated 180
  // degrees, because that is how the device reads it.
  //
  // The picture and the JPEG live in PSRAM - the internal heap is nearly
  // full and a picture does not fit in it.
#if HIDCTL_HAS_JPEG
  // ---- the picture push, as a state machine ---------------------------
  //
  // The deck's OUT pipe takes ONE transfer at a time, and a picture is
  // three of them (the BAT header, the image chunk and the commit), so
  // the push cannot be a burst: everything but the F1's single LED report
  // would be refused. One packet goes out per pass of the main loop, and
  // the picture is encoded one key at a time so the main loop is not held
  // up by ten JPEG encodings in a row.
  static constexpr uint8_t OPENDECK_KEYS = 10;
  static constexpr uint8_t OPENDECK_ZONES = 4;    // the touch strip's four zones
  static constexpr uint8_t OPENDECK_PICS = OPENDECK_KEYS + OPENDECK_ZONES;

  struct OpendeckPic {
    uint8_t  slot;        // the BAT slot
    uint8_t  colour[3];   // blue, green, red
    uint16_t w, h;        // the picture's size
    uint16_t number;      // the number to draw on it, 0 = none
  };
  OpendeckPic opendeck_pics[OPENDECK_PICS] = {};
  uint8_t opendeck_queue_count = 0;
  uint8_t opendeck_queue_pos = 0;
  uint16_t opendeck_jpeg_len = 0;
  uint16_t opendeck_chunk_off = 0;
  bool opendeck_painting = false;

  // Work out what each screen should show.
  //
  // The ten keys with displays get the same categories the F1's pads use -
  // green for the preset that is running, blue for one that is saved, red
  // or magenta for a playlist, yellow for the music playlist.
  //
  // The wide touch strip is four zones, one per encoder, and they get a
  // colour each so the encoders can be told apart. The reference calls
  // these the "dial" images: they are what shows what each rotary
  // encoder is doing.
  void opendeckBuildQueue() {
    opendeck_queue_count = 0;
    const uint8_t keys =
        (active_profile < profile_count && profiles[active_profile].od_keys)
            ? profiles[active_profile].od_keys : OPENDECK_KEYS;
    for (uint8_t i = 0; i < keys && i < OPENDECK_KEYS; i++) {
      const uint8_t slot = opendeckSlotFor(i);
      if (!slot) continue;
      // The key shows the same preset the pad of the F1 would: the one at
      // this key's place in the CURRENT PAGE. Using the key's own number
      // instead would mean the active preset could never be highlighted
      // when it lives past the first ten.
      const int slot_no = (int)pageOffset() + (int)i + 1;
      uint8_t r = 0, g = 0, b = 0;
      const bool is_music = (music_slot > 0 && slot_no == (int)music_slot);
      const bool is_playlist = getCachedPresetExists((uint8_t)slot_no)
                            && presetCache != nullptr && presetCache[slot_no].isPlaylist;
      if (is_music) {
        r = 0x7F; g = 0x7F;                      // yellow
      } else if (is_playlist) {
        const bool finite = getPresetRepeat((uint8_t)slot_no) > 0;
        if (finite) r = 0x7F; else { r = 0x7F; b = 0x7F; }
      } else if (slot_no == (int)tracked_active_preset
                 && getCachedPresetExists((uint8_t)slot_no)) {
        g = 0x7F;                                 // green
      } else if (getCachedPresetExists((uint8_t)slot_no)) {
        b = 0x7F;                                 // blue
      }
      // A key that is blinking: white in its "on" half, or its own colour
      // with the off half dark (the playlist chaser). The F1's pads use
      // the same rules.
      {
        char nm[HIDCTL_MAX_NAME];
        const HidctlButton* btn = buttonByIndex(i);
        snprintf(nm, sizeof(nm), "%s", btn ? btn->name : "");
        const int phase = blinkPhase(nm);
        if (phase == 0) {
          b = r = g = 0;
        } else if (phase == 1 && !blinkKeepsColour(nm)) {
          r = g = b = 0x7F;
        }
      }
      OpendeckPic& pic = opendeck_pics[opendeck_queue_count];
      pic.slot = slot;
      pic.colour[0] = b; pic.colour[1] = g; pic.colour[2] = r;
      pic.w = Opendeck::KEY_W; pic.h = Opendeck::KEY_H;
      // The number on the key is the slot it holds, so the deck reads as
      // a bank of presets.
      pic.number = (uint16_t)slot_no;
      opendeck_queue_count++;
    }
    // The wide touch strip. On a deck whose strip is ONE screen it is a
    // single picture across all of it; on the others it is four zones,
    // one per encoder, and they are the reference's "dial" images.
    const bool strip_one = (active_profile < profile_count && profiles)
                           && profiles[active_profile].od_strip_one;
    if (strip_one) {
      OpendeckPic& pic = opendeck_pics[opendeck_queue_count];
      pic.slot = 0;                          // slot 0 is the whole strip
      pic.colour[0] = pic.colour[1] = pic.colour[2] = 0;
      pic.w = Opendeck::STRIP_W; pic.h = Opendeck::STRIP_H;
      pic.number = 0;
      opendeck_queue_count++;
    } else {
      for (uint8_t i = 0; i < OPENDECK_ZONES; i++) {
        const uint8_t hue = i * 36;
        OpendeckPic& pic = opendeck_pics[opendeck_queue_count];
        pic.slot = (uint8_t)(i + 1);            // the strip zones are slots 1..4
        pic.colour[0] = (uint8_t)(127 * (1 + 0.9 * __builtin_cosf(3.14159f * (hue + 240) / 180)));
        pic.colour[1] = (uint8_t)(127 * (1 + 0.9 * __builtin_cosf(3.14159f * (hue + 120) / 180)));
        pic.colour[2] = (uint8_t)(127 * (1 + 0.9 * __builtin_cosf(3.14159f * hue / 180)));
        pic.w = Opendeck::DIAL_W; pic.h = Opendeck::DIAL_H;
        pic.number = (uint16_t)(i + 1);         // the encoder's number
        opendeck_queue_count++;
      }
    }
    opendeck_queue_pos = 0;
    opendeck_jpeg_len = 0;
    opendeck_chunk_off = 0;
    opendeck_stage = OPENDECK_STAGE_ENCODE;
  }

  // Draw a number centred on a picture. The picture is in the encoder's
  // order (blue first), so the colour is given that way too.
  void opendeckDrawNumber(uint8_t* bgr, uint16_t w, uint16_t h, uint16_t number,
                          uint8_t blue, uint8_t green, uint8_t red) {
    char text[4];
    snprintf(text, sizeof(text), "%u", number);
    const uint8_t n = (uint8_t)strlen(text);
    if (!n) return;
    // Six columns per digit (five plus a gap), seven rows.
    const uint16_t units_w = (uint16_t)(n * 6 - 1);
    uint16_t scale = (uint16_t)(w / (units_w + 1));
    uint16_t vscale = (uint16_t)(h / 8);
    if (vscale < scale) scale = vscale;
    if (!scale) return;
    const uint16_t glyph_w = (uint16_t)(5 * scale);
    const uint16_t glyph_h = (uint16_t)(7 * scale);
    const uint16_t gap = scale;
    const uint16_t text_w = (uint16_t)(n * glyph_w + (n - 1) * gap);
    const uint16_t x0 = (uint16_t)((w > text_w) ? (w - text_w) / 2 : 0);
    const uint16_t y0 = (uint16_t)((h > glyph_h) ? (h - glyph_h) / 2 : 0);

    for (uint8_t d = 0; d < n; d++) {
      const uint8_t digit = (uint8_t)(text[d] - '0');
      if (digit > 9) continue;
      for (uint8_t row = 0; row < 7; row++) {
        const uint8_t bits = Opendeck::DIGIT_FONT[digit][row];
        for (uint8_t col = 0; col < 5; col++) {
          if (!(bits & (0x10 >> col))) continue;
          const uint16_t px = (uint16_t)(x0 + d * (glyph_w + gap) + col * scale);
          const uint16_t py = (uint16_t)(y0 + row * scale);
          for (uint16_t sy = 0; sy < scale; sy++) {
            uint8_t* p = bgr + ((size_t)(py + sy) * w + px) * 3;
            for (uint16_t sx = 0; sx < scale; sx++) {
              p[0] = blue; p[1] = green; p[2] = red;
              p += 3;
            }
          }
        }
      }
    }
  }

  // Draw a number centred on a rectangle of a picture, with the rectangle
  // at (x0, y0) and the given size. The wide strip's tiles use it.
  void opendeckDrawNumberIn(uint8_t* bgr, uint16_t w, uint16_t h,
                            uint16_t x0, uint16_t y0, uint16_t rw, uint16_t rh,
                            uint16_t number, uint8_t blue, uint8_t green, uint8_t red) {
    char text[4];
    snprintf(text, sizeof(text), "%u", number);
    const uint8_t n = (uint8_t)strlen(text);
    if (!n) return;
    const uint16_t units_w = (uint16_t)(n * 6 - 1);
    uint16_t scale = (uint16_t)(rw / (units_w + 1));
    uint16_t vscale = (uint16_t)(rh / 8);
    if (vscale < scale) scale = vscale;
    if (!scale) return;
    const uint16_t glyph_w = (uint16_t)(5 * scale);
    const uint16_t glyph_h = (uint16_t)(7 * scale);
    const uint16_t gap = scale;
    const uint16_t text_w = (uint16_t)(n * glyph_w + (n - 1) * gap);
    const uint16_t tx = (uint16_t)(x0 + (rw > text_w ? (rw - text_w) / 2 : 0));
    const uint16_t ty = (uint16_t)(y0 + (rh > glyph_h ? (rh - glyph_h) / 2 : 0));

    for (uint8_t d = 0; d < n; d++) {
      const uint8_t digit = (uint8_t)(text[d] - '0');
      if (digit > 9) continue;
      for (uint8_t row = 0; row < 7; row++) {
        const uint8_t bits = Opendeck::DIGIT_FONT[digit][row];
        for (uint8_t col = 0; col < 5; col++) {
          if (!(bits & (0x10 >> col))) continue;
          const uint16_t px = (uint16_t)(tx + d * (glyph_w + gap) + col * scale);
          const uint16_t py = (uint16_t)(ty + row * scale);
          if (px + scale > w || py + scale > h) continue;
          for (uint16_t sy = 0; sy < scale; sy++) {
            uint8_t* p = bgr + ((size_t)(py + sy) * w + px) * 3;
            for (uint16_t sx = 0; sx < scale; sx++) {
              p[0] = blue; p[1] = green; p[2] = red;
              p += 3;
            }
          }
        }
      }
    }
  }

  // The wide touch strip is ONE screen: 800 by 128 pixels, cut into a
  // 200 by 128 tile per encoder. The reference composites the four
  // encoder pictures into it and sends it as one "BGPIC".
  //
  // The canvas is built here, in PSRAM, and the JPEG comes out of it.
  bool opendeckEncodeStrip(uint8_t*& jpeg_out, uint16_t& jpeg_len_out) {
    jpeg_out = nullptr;
    jpeg_len_out = 0;
    const uint16_t w = Opendeck::STRIP_W, h = Opendeck::STRIP_H;
    const size_t pic_len = (size_t)w * h * 3;
    uint8_t* buf = opendeckAlloc(pic_len);
    if (!buf) return false;
    uint8_t* jpeg = opendeckAlloc(pic_len + 16384);
    if (!jpeg) { free(buf); return false; }

    // Black, then a coloured tile per encoder with its number on it.
    memset(buf, 0, pic_len);
    for (uint8_t i = 0; i < Opendeck::STRIP_TILES; i++) {
      const uint8_t hue = i * 36;
      const uint8_t b = (uint8_t)(127 * (1 + 0.9 * __builtin_cosf(3.14159f * (hue + 240) / 180)));
      const uint8_t g = (uint8_t)(127 * (1 + 0.9 * __builtin_cosf(3.14159f * (hue + 120) / 180)));
      const uint8_t r = (uint8_t)(127 * (1 + 0.9 * __builtin_cosf(3.14159f * hue / 180)));
      const uint16_t x0 = (uint16_t)(i * Opendeck::STRIP_TILE_W);
      for (uint16_t y = 0; y < h; y++) {
        uint8_t* p = buf + ((size_t)y * w + x0) * 3;
        for (uint16_t x = 0; x < Opendeck::STRIP_TILE_W; x++) {
          p[0] = b; p[1] = g; p[2] = r;
          p += 3;
        }
      }
      opendeckDrawNumberIn(buf, w, h, x0, 0, Opendeck::STRIP_TILE_W, h,
                            (uint16_t)(i + 1), 0xFF, 0xFF, 0xFF);
    }

    // The device reads its pictures upside down.
    {
      const size_t row = (size_t)w * 3;
      uint8_t* tmp = opendeckAlloc(row);
      if (tmp) {
        for (uint16_t y = 0; y < h; y++) {
          uint8_t* src = buf + (size_t)(h - 1 - y) * row;
          uint8_t* dst = buf + (size_t)y * row;
          memcpy(tmp, src, row);
          for (uint16_t x = 0; x < w; x++) {
            memcpy(dst + (size_t)x * 3, tmp + (size_t)(w - 1 - x) * 3, 3);
          }
        }
        free(tmp);
      }
    }

    size_t jpeg_len = 0;
    const bool ok = opendeckEncodeJpeg(buf, w, h, jpeg, pic_len + 16384, jpeg_len);
    free(buf);
    if (!ok) { free(jpeg); return false; }
    if (opendeck_jpeg) free(opendeck_jpeg);
    opendeck_jpeg = jpeg;
    jpeg_out = jpeg;
    jpeg_len_out = (uint16_t)jpeg_len;
    return true;
  }

  // Draw one picture and encode it. The picture is in the encoder's
  // order (blue first) and turned 180 degrees, because that is how the
  // device reads it.
  bool opendeckEncodeKey(uint8_t index) {
    const OpendeckPic& pic = opendeck_pics[index];
    const uint16_t w = pic.w, h = pic.h;
    const size_t pic_len = (size_t)w * h * 3;

    uint8_t* buf = opendeckAlloc(pic_len);
    if (!buf) return false;
    uint8_t* jpeg = opendeckAlloc(pic_len + 8192);
    if (!jpeg) { free(buf); return false; }

    uint8_t* px = buf;
    for (size_t n = 0; n < pic_len; n += 3) {
      *px++ = pic.colour[0]; *px++ = pic.colour[1]; *px++ = pic.colour[2];
    }
    // The slot number on the picture: a row of coloured squares is no
    // use as a preset bank.
    if (pic.number) {
      opendeckDrawNumber(buf, w, h, pic.number, 0xFF, 0xFF, 0xFF);
    }
    // The device reads its pictures upside down: a 180 degree turn, so the
    // rows and the columns both go.
    const size_t row = (size_t)w * 3;
    uint8_t* tmp = opendeckAlloc(row);
    if (tmp) {
      for (uint16_t y = 0; y < h; y++) {
        uint8_t* src = buf + (size_t)(h - 1 - y) * row;
        uint8_t* dst = buf + (size_t)y * row;
        memcpy(tmp, src, row);
        for (uint16_t x = 0; x < w; x++) {
          memcpy(dst + (size_t)x * 3, tmp + (size_t)(w - 1 - x) * 3, 3);
        }
      }
      free(tmp);
    }

    size_t jpeg_len = 0;
    const bool ok = opendeckEncodeJpeg(buf, w, h, jpeg, pic_len + 8192, jpeg_len);
    free(buf);
    if (!ok) { free(jpeg); return false; }

    // The JPEG has to outlive this call: the chunks go out over the next
    // passes of the loop.
    if (opendeck_jpeg) free(opendeck_jpeg);
    opendeck_jpeg = jpeg;
    opendeck_jpeg_len = (uint16_t)jpeg_len;
    return true;
  }

  uint8_t* opendeck_jpeg = nullptr;

  // One packet per pass of the main loop.
  //
  // A picture takes three steps and each one is a separate pass, because
  // the OUT pipe takes one transfer at a time: make the picture, announce
  // it (the "BAT" header, which names the key and the length), and then
  // send it (the chunks). The steps have to be kept apart: if the "BAT"
  // header is built while the pipe is busy and the picture is sent
  // anyway, the device gets the image with no header and does not know
  // which key it is for.
  static constexpr uint8_t OPENDECK_STAGE_ENCODE = 0;
  static constexpr uint8_t OPENDECK_STAGE_BAT    = 1;
  static constexpr uint8_t OPENDECK_STAGE_CHUNK  = 2;
  uint8_t opendeck_stage = OPENDECK_STAGE_ENCODE;

  void opendeckPaintStep() {
    if (!opendeck_painting) return;
    if (opendeck_queue_pos >= opendeck_queue_count) {
      // The last picture is away: commit the batch. Without it nothing
      // appears on the keys.
      Opendeck::stp(led_report);
      if (opendeckSend(led_report)) {
        opendeck_painting = false;
        USER_PRINTF("[HIDCTL] deck: %u pictures are away\n", opendeck_queue_count);
      }
      return;
    }
    if (opendeck_stage == OPENDECK_STAGE_ENCODE) {
      // This screen's picture has to be made first.
      const bool strip = opendeck_pics[opendeck_queue_pos].slot == 0;
      const bool ok = strip ? opendeckEncodeStrip(opendeck_jpeg, opendeck_jpeg_len)
                            : opendeckEncodeKey(opendeck_queue_pos);
      if (!ok) {
        opendeck_queue_pos++;
        return;
      }
      opendeck_stage = OPENDECK_STAGE_BAT;
      return;
    }
    if (opendeck_stage == OPENDECK_STAGE_BAT) {
      const OpendeckPic& pic = opendeck_pics[opendeck_queue_pos];
      if (pic.slot == 0) {
        // The wide strip: one picture across all of it.
        Opendeck::bgpic(led_report, opendeck_jpeg_len, 0, 0, pic.w, pic.h);
      } else {
        Opendeck::bat(led_report, (uint8_t)(pic.slot - 1), opendeck_jpeg_len);
      }
      if (!opendeckSend(led_report)) return;    // the pipe is busy: try again
      USER_PRINTF("[HIDCTL] deck: picture for slot %u (%ux%u, %u bytes)\n",
                  pic.slot, pic.w, pic.h, opendeck_jpeg_len);
      opendeck_chunk_off = 0;
      opendeck_stage = OPENDECK_STAGE_CHUNK;
      return;
    }
    if (opendeck_chunk_off < opendeck_jpeg_len) {
      uint16_t n = (uint16_t)(opendeck_jpeg_len - opendeck_chunk_off);
      if (n > Opendeck::PACKET_SIZE) n = Opendeck::PACKET_SIZE;
      memset(led_report, 0, Opendeck::WIRE_LEN);
      memcpy(led_report + 1, opendeck_jpeg + opendeck_chunk_off, n);
      if (!opendeckSend(led_report)) return;    // the pipe is busy: try again
      opendeck_chunk_off += n;
      return;
    }
    // This key is done: on to the next one.
    opendeck_queue_pos++;
    opendeck_jpeg_len = 0;
    opendeck_stage = OPENDECK_STAGE_ENCODE;
  }

  void opendeckPaintKeys() {
    if (opendeck_painting) return;         // one push at a time
    opendeckBuildQueue();
    opendeck_painting = true;
  }

  // The playlist chaser for a deck: the key that holds the running
  // playlist pulses slowly and the key that holds the track that is
  // playing blinks fast - the same pattern the MIDI usermod uses and the
  // F1's pads follow.
  void opendeckChaser() {
    if (active_profile >= profile_count || !profiles) return;
    HidctlProfile& p = profiles[active_profile];
    if (!p.opendeck) return;
    const bool playlist_active = (currentPlaylist >= 0);
    const int parent = playlist_active ? (int)currentPlaylist : 0;
    const int child = (int)tracked_active_preset;
    for (uint8_t i = 0; i < p.button_count; i++) {
      const HidctlButton& b = p.buttons[i];
      if (b.index >= 10 || !b.name[0]) continue;    // only the keys with screens
      const int slot = (int)pageOffset() + (int)b.index + 1;
      if (slot < 1 || slot > 250) continue;
      if (slot == parent) {
        blinkSet(b.name, 1.0f, 1000, true);        // the parent: a slow pulse
      } else if (slot == child) {
        blinkSet(b.name, 4.0f, 1000, true);        // the child: a fast blink
      }
    }
  }
#else
  // No hardware encoder on this target, so there are no pictures.
  void opendeckPaintKeys() {}
  void opendeckChaser() {}
#endif

  // Decode a raw report for the active profile and act on it.
  void handleIncomingReport(const uint8_t* data, uint16_t len) {
    if (!enabled || active_profile >= profile_count || !profiles) return;
    HidctlProfile& p = profiles[active_profile];

    // Record what arrived, whatever it is: a report the profile does not
    // expect is still a report, and a short one is still a report.
    recordReport(data, len);

    // An OpenDeck-family deck is not a plain HID device: its reports are
    // single events, and its output is a command protocol.
    if (p.opendeck) {
      handleOpendeckReport(p, data, len);
      return;
    }

    // The wheel's byte, in any report: if it ever moves, the encoder is
    // being reported in something.
    if (p.wheel_present && len > p.wheel_offset) {
      uint8_t pos = data[p.wheel_offset];
      if (pos < wheel_min) wheel_min = pos;
      if (pos > wheel_max) wheel_max = pos;
      if (pos != 0) wheel_moved = true;
    }

    if (len < p.report_len) return;
    if (p.report_id && data[0] != p.report_id) {
      // A report we did not ask for. Say so: the device may send the
      // encoder's position in a report of its own.
      USER_PRINTF("[HIDCTL] report id 0x%02X (%u bytes): %s\n", data[0], len,
                  bytes_to_hex(data, len < 8 ? len : 8));
      return;
    }

    // --- buttons ------------------------------------------------------------
    // The button bitmap is read BEFORE the controls are applied: a report
    // that carries a modifier AND a control has to see the modifier, or
    // SHIFT + knob4 would drive the palette as well as the controller's
    // brightness.
    uint32_t now_buttons = 0;
    for (uint8_t bi = 0; bi < p.button_byte_count; bi++) {
      uint8_t byte = data[p.button_bytes[bi]];
      for (uint8_t bit = 0; bit < 8; bit++) {
        bool set = p.msb_first ? (byte & (0x80 >> bit)) : (byte & (1 << bit));
        if (set) now_buttons |= (1UL << (bi * 8 + bit));
      }
    }
    uint32_t was_buttons = buttons;
    buttons = now_buttons;

    // Modifiers are buttons whose action is only meaningful while held.
    modifier_mask = 0;
    for (uint8_t i = 0; i < p.button_count; i++) {
      if (p.buttons[i].modifier) modifier_mask |= (1UL << p.buttons[i].index);
    }
    bool mod_held = (now_buttons & modifier_mask) != 0;

    // --- continuous controls ------------------------------------------------
    // A value that arrived while the throttle was closed goes first: it is
    // the newest one, and the knob may already have stopped there.
    flushPendingControls();

    const uint32_t now = millis();
    for (uint8_t i = 0; i < p.control_count; i++) {
      HidctlControl& ctl = p.controls[i];
      uint16_t raw = 0;
      uint8_t nbytes = (ctl.bits + 7) / 8;
      for (uint8_t b = 0; b < nbytes; b++) {
        raw |= (uint16_t)((uint16_t)data[ctl.offset + b] << (8 * b));   // little endian
      }
      if (ctl.bits < 16) raw &= (uint16_t)((1u << ctl.bits) - 1);
      if (control_has_applied[i] && raw == control_value[i]) continue;  // no change at all
      control_value[i] = raw;

      // A step or two of jitter is not a movement. While the change stays
      // inside the deadband the baseline stays pinned where it was, so a
      // slow drift still adds up to a movement - it is measured against
      // the last value that passed the deadband, not against the last
      // value seen.
      if (control_has_applied[i]) {
        int d = (int)raw - (int)control_applied[i];
        if (d < 0) d = -d;
        if (d < CONTROL_DEADBAND) {
          control_pending[i] = false;
          continue;
        }
      }

      // Too soon after the last one. The next report carries a newer
      // value, and that is the one worth driving.
      if (control_has_applied[i] &&
          (now - control_applied_ms[i]) < CONTROL_APPLY_INTERVAL_MS) {
        control_pending[i] = true;
        continue;
      }

      applyControl(p, i, raw, now);
      control_pending[i] = false;
    }

    // --- the encoder -------------------------------------------------------
    // Its byte counts the wheel's position, so the difference says which
    // way it turned. One step is one page.
    if (p.wheel_present && len > p.wheel_offset) {
      uint8_t pos = data[p.wheel_offset];
      if (wheelSeen) {
        int8_t delta = (int8_t)(pos - wheelLast);   // the counter wraps
        if (delta != 0) {
          USER_PRINTF("[HIDCTL] wheel %u -> %u (delta %d)\n",
                      wheelLast, pos, (int)delta);
          showWheelUntil = millis() + 1000;   // show it on the display
          turnPage(delta);
        }
      } else {
        USER_PRINTF("[HIDCTL] wheel first value %u\n", pos);
      }
      wheelLast = pos;
      wheelSeen = true;
    }

    // A heartbeat of the report's first bytes, so the wire can be watched.
    // It is slow on purpose: the console is read by a person.
    static uint32_t lastBeat = 0;
    if (millis() - lastBeat > 10000) {
      lastBeat = millis();
      USER_PRINTF("[HIDCTL] report: %s\n", bytes_to_hex(data, 8));
    }

    uint32_t pressed = now_buttons & ~was_buttons;

    if (pressed) {
      for (uint8_t i = 0; i < p.button_count; i++) {
        const HidctlButton& btn = p.buttons[i];
        if (btn.modifier) continue;              // modifiers act on release
        if (!(pressed & (1UL << btn.index))) continue;
        const char* act = (mod_held && btn.mod_action[0]) ? btn.mod_action : btn.action;

        char shifted[HIDCTL_MAX_ACTION + 8];
        const char* use = withPageOffset(act, shifted, sizeof(shifted));
        USER_PRINTF("[HIDCTL] %s -> %s\n", btn.name, use);
        runAction(use, 255);
      }
      // The LEDs may have changed (a button lights while it is held).
      paint();
    }
  }

  // Current value of the parameter an action names, or -1 if it is not a
  // parameter (soft takeover needs to know).
  int currentValueOf(const char* action) const {
    if (!strcmp(action, "bri")) return bri;
    if (!strcmp(action, "effectSpeed")) return effectSpeed;
    if (!strcmp(action, "effectIntensity")) return effectIntensity;
    if (!strcmp(action, "effectPalette")) return effectPalette;
    if (!strcmp(action, "effectCustom1")) return strip.getMainSegment().custom1;
    if (!strcmp(action, "effectCustom2")) return strip.getMainSegment().custom2;
    if (!strncmp(action, "seg", 3) && isdigit(action[3])) {
      uint8_t seg = (uint8_t)(action[3] - '0');
      if (!strcmp(action + 4, "bri")) {
        if (seg < strip.getSegmentsNum()) return strip.getSegment(seg).opacity;
      }
    }
    return -1;
  }

  // -------------------------------------------------------------------------
  // Usermod boilerplate
  // -------------------------------------------------------------------------
  void setup() override {
    writeDefaultProfileIfMissing(HIDCTL_PROFILE_DIR HIDCTL_DEFAULT_PROFILE_FILE,
                                 defaultProfileText());
    // The OpenDeck-family deck, as a second profile.
    writeDefaultProfileIfMissing(HIDCTL_PROFILE_DIR "hidctl-soomfon-cn003.json",
                                 HIDCTL_CN003_PROFILE_JSON);
    loadProfiles();
  }

  void loop() override {
    // A heartbeat, so it is visible whether this loop is running at all.
    {
      static uint32_t lastBeat = 0;
      if (millis() - lastBeat > 10000) {
        lastBeat = millis();
        USER_PRINTF("[HIDCTL] loop: active=%u count=%u opendeck=%u pending=%u\n",
                    active_profile, profile_count,
                    (active_profile < profile_count && profiles)
                        ? (profiles[active_profile].opendeck ? 1 : 0) : 0,
                    opendeck_pending_init ? 1 : 0);
      }
    }
    // A control value held back by the throttle is applied here too, in
    // case the controller has stopped reporting.
    flushPendingControls();
    // The beat counter on the lower buttons moves on its own, so the LEDs
    // have to be repainted even when nothing else changes. paint() rate
    // limits itself.
    paint();
    // The LED report goes out from here and nowhere else: this is the
    // task that owns the USB host stack.
    flushLedReport();
    // The zero-length packet that ends a report goes out from here too:
    // a transfer submitted from inside the completion of another one is
    // what the USB stack here does not survive.
    hid_usb_pump_out();
    // An OpenDeck-family deck: its session and its pictures are handled
    // here, because this is the task that owns the USB host stack.
    if (active_profile < profile_count && profiles[active_profile].opendeck) {
      // The chaser sets the blinks, and a blink on a deck is in the
      // PICTURE, so the pictures have to go out again at the blink's rate.
      opendeckChaser();
      {
        static uint32_t last_deck_push = 0;
        if (blinkActive() && !opendeck_pending_init && !opendeck_painting &&
            (uint32_t)(millis() - last_deck_push) > 60) {
          last_deck_push = millis();
          opendeck_pending_paint = true;
        }
      }
      if (opendeck_pending_init) {
        opendeck_pending_init = false;
        opendeck_pending_paint = false;
        opendeckInit();
        opendeckPaintKeys();
      } else if (opendeck_pending_paint) {
        opendeck_pending_paint = false;
        opendeckPaintKeys();
      }
      // The commands and the pictures go out one packet at a time: the
      // OUT pipe takes one transfer at a time.
      if (!opendeckCmdPending()) opendeckPaintStep();
      else opendeckFlushCmd();
      opendeckKeepalive();
    }
    // Commands from the web API that touch the USB.
    if (cmd_wakeup) {
      cmd_wakeup = false;
      hid_usb_send_wakeup();
    }
    if (cmd_other_iface >= 0) {
      int8_t claim = cmd_other_iface;
      cmd_other_iface = -1;
      hid_usb_set_other_interface(claim != 0);
    }
  }

  // The preset changed: the pads and the display have to follow.
  void onStateChange(uint8_t mode) override {
    paint();
    // A deck's screens are pictures, so they have to be pushed again.
    repaintDeck();
  }

  // The core fires this BEFORE it wipes currentPreset to 0, so the active
  // preset is read here and kept. Without it the green highlight on the
  // active pad would vanish the moment anything changed.
  void onPreStateChange(uint8_t mode) override {
    if (currentPreset != 0) tracked_active_preset = currentPreset;
  }

  void onEvent(const wled::Event& ev) override {
    if (ev.type == wled::EventType::PresetApplied) {
      // The core says a preset was applied, and which one. This is the
      // authoritative answer: the core's own currentPreset is wiped to 0
      // as soon as anything changes, so it cannot be read later.
      uint8_t slot = ev.payload.presetApplied.preset;
      if (slot >= 1) {
        tracked_active_preset = slot;
        // The pads address the page the active preset is on.
        page = pageOfPreset(slot);
        USER_PRINTF("[HIDCTL] preset %u -> page %u (cached=%u playlist=%u music=%u)\n",
                    slot, page, getCachedPresetExists(slot) ? 1 : 0,
                    (presetCache != nullptr && presetCache[slot].isPlaylist) ? 1 : 0,
                    music_slot);
        paint();
        repaintDeck();
      }
      return;
    }
    if (ev.type == wled::EventType::UsbDeviceChanged) {
      bool was = active_profile != 0xFF;
      if (ev.payload.usbDevice.connected) {
        uint8_t idx = matchDevice(ev.payload.usbDevice.vid, ev.payload.usbDevice.pid);
        if (idx != 0xFF) {
          active_profile = idx;
          resetControls();
          buttons = 0;
          // The wheel's position belongs to the connection: forget the old
          // one so the first report after a reconnect is not read as a turn.
          wheelSeen = false;
          wheelLast = 0;
          USER_PRINTF("[HIDCTL] %s connected\n", profiles[idx].name);
          // An OpenDeck-family deck needs its session opened: DIS resets
          // the display, LIG sets the backlight, and CONNECT keeps the
          // session alive. Its keys are LCDs, so they are given pictures.
          if (profiles[idx].opendeck) {
            opendeck_pending_init = true;
            opendeck_pending_paint = true;
          }
        }
      } else {
        active_profile = 0xFF;
      }
      if (was != (active_profile != 0xFF)) {
        USER_PRINTF("[HIDCTL] control surface %s\n",
                    active_profile != 0xFF ? "connected" : "disconnected");
      }
    }
  }

  // The LED demo can be switched on and off from the web API, without a
  // rebuild:
  //   POST /json/state  {"HIDCTL":{"demo":true}}
  // "paint" turns the LED painting off and on - useful to see whether the
  // device behaves differently when nothing is written to it.
  // "page" jumps to a page of presets, 1 to 16. The encoder turns the
  // page too, but it does not always reach the board, so the page can be
  // set from here as well:
  //   POST /json/state  {"HIDCTL":{"page":10}}
  void readFromJsonState(JsonObject& obj) override {
    JsonObject h = obj[FPSTR(_name)];
    if (h.isNull()) return;
    bool demo = h["demo"] | false;
    setDemo(demo);
    if (h.containsKey("paint")) paint_enabled = h["paint"] | true;
    // Send the output report over the control pipe (SET_REPORT) instead
    // of the interrupt OUT endpoint. Some devices only take it that way.
    if (h.containsKey("ctrlout")) hid_usb_set_out_via_control(h["ctrlout"] | false);
    // Send the packet without its leading report-ID byte.
    if (h.containsKey("nolead")) hid_usb_set_out_drop_leading(h["nolead"] | false);
    // End the packet with a zero-length packet. A device whose report is
    // exactly the size of its endpoint's maximum packet needs it.
    if (h.containsKey("zlp")) hid_usb_set_out_zlp(h["zlp"] | false);
    // Move the report to the front of the packet and keep the packet's
    // length, so a pad byte ends the transfer.
    if (h.containsKey("shift")) hid_usb_set_out_shift(h["shift"] | false);
    // Cap how many bytes of the packet go out. The deck's report is 1024
    // bytes and its endpoint's maximum packet is 1024, so the 1025-byte
    // packet goes out as 1024 and then a short packet of one byte. A
    // Windows HID stack drops that trailing byte, so the device may only
    // ever see 1024 - and a short packet in the middle of a stream may
    // throw its parser out.
    if (h.containsKey("outcap")) hid_usb_set_out_cap((uint16_t)(h["outcap"] | 0));
    // The gap between two packets to the deck, in ms. A PC's HID stack
    // leaves 20 ms between them and the deck may need the room.
    if (h.containsKey("outgap")) opendeck_out_gap = (uint32_t)(h["outgap"] | 0);
    // Push the pictures again. NOT a new session: the reference sends its
    // DIS once per connection and the device does not like it repeated.
    if (h["repaint"]) {
      opendeck_pending_paint = true;
    }
    // Write the built-in profiles over the ones on the filesystem and read
    // them back. The defaults are only written when the file is missing,
    // so a profile written by an older firmware keeps its old contents.
    if (h["writedefaults"]) {
      writeDefaultProfiles();
    }
    // How many bytes the interrupt IN transfer asks for, and a forced
    // re-arm of the IN pipe. Both are diagnostics for the deck's input.
    if (h.containsKey("inlen")) hid_usb_set_in_xfer_len((uint16_t)(h["inlen"] | 0));
    if (h["inreset"]) hid_usb_reset_in_pipe();
    // Put the LED report on the control pipe instead of the interrupt OUT
    // endpoint. The F1 takes it on the OUT endpoint, so this is a
    // diagnostic.
    if (h.containsKey("ledout")) led_out_endpoint = h["ledout"] | true;
    // Send the class requests again - the vendor request and SET_IDLE that
    // a PC's HID stack sends and this host did not. They are off by
    // default: the F1 stalls them, and this host does not clear the stall.
    // Blink a light, or the display (an empty name):
    //   POST /json/state  {"HIDCTL":{"blink":"sync","rate":2,"ms":3000}}
    if (h.containsKey("blink")) {
      const char* what = h["blink"] | "";
      float rate = h["rate"] | 1.0f;
      uint32_t ms = h["ms"] | 1000;
      blink(what, rate, ms);
    }
    if (h.containsKey("classreq")) hid_usb_set_class_requests(h["classreq"] | false);
    // Claim or release the device's other interface. A PC's HID stack
    // leaves the F1's second interface alone; we claimed it on a hunch.
    // The web API runs in the async_tcp task and the USB stack belongs to
    // the main loop, so the command is carried there.
    if (h.containsKey("otheriface")) cmd_other_iface = (h["otheriface"] | true) ? 1 : 0;
    // Send the wake-up packet again - also carried to the main loop.
    if (h["a0"]) cmd_wakeup = true;
    // Ask for the feature reports (a PC's HID stack does not).
    if (h.containsKey("featrep")) hid_usb_set_feature_reports(h["featrep"] | true);
    if (h.containsKey("page")) {
      int pg = h["page"] | 0;
      if (pg >= 1 && pg <= (int)lastPage()) {
        page = (uint8_t)pg;
        showPageUntil = millis() + 1500;
        paint();
      }
    }
  }

  void addToJsonInfo(JsonObject& obj) override {
    // The info pane's rows are flat: each usermod publishes its own key in
    // "u", and only the first two elements of a row are rendered.
    JsonObject user = obj["u"];
    if (user.isNull()) user = obj.createNestedObject("u");
    JsonArray infoArr = user.createNestedArray(FPSTR(_name));
    if (active_profile != 0xFF) {
      infoArr.add(profileName());
      char buf[32];
      snprintf(buf, sizeof(buf), "%u profiles", profile_count);
      infoArr.add(buf);
    } else {
      infoArr.add("disconnected");
    }
  }

  void addToJsonAr(JsonObject& obj) override {
    JsonObject top = obj.createNestedObject(FPSTR(_name));
    top["connected"] = (active_profile != 0xFF);
    top["device"] = profileName();
    top["profiles_loaded"] = profile_count;
    top["buttons"] = buttons;
    // How many of the first 64 preset slots the core's cache knows about -
    // the pads are coloured from it.
    uint8_t cached = 0;
    for (uint8_t s = 1; s <= 64; s++) {
      if (getCachedPresetExists(s)) cached++;
    }
    top["presets_cached"] = cached;
    top["music_slot"] = music_slot;
    top["demo"] = demoActive;
    top["class_status"] = hid_usb_class_status();
    top["class_stage"] = hid_usb_class_stage();
    top["other_iface"] = hid_usb_other_iface();
    top["other_claimed"] = hid_usb_other_claimed();
    {
      uint8_t fl = 0;
      const uint8_t* fd = hid_usb_feature_data(&fl);
      if (fl) {
        char hex[2 * 72 + 1];
        int hl = 0;
        for (uint8_t i = 0; i < fl && hl < (int)sizeof(hex) - 3; i++) {
          hl += snprintf(hex + hl, sizeof(hex) - hl, "%02X", fd[i]);
        }
        top["feature"] = hex;
        top["feature_len"] = fl;
      }
    }
    {
      uint8_t cl = 0;
      const uint8_t* cd = hid_usb_class_data(&cl);
      if (cl) {
        char hex[2 * 8 + 1];
        int hl = 0;
        for (uint8_t i = 0; i < cl && hl < (int)sizeof(hex) - 3; i++) {
          hl += snprintf(hex + hl, sizeof(hex) - hl, "%02X", cd[i]);
        }
        top["class_data"] = hex;
      }
    }
    top["wheel"] = wheelSeen ? wheelLast : 0;
    top["wheel_seen"] = wheelSeen;
    // The lowest and highest value the wheel byte has ever had, and
    // whether it has ever been anything but zero.
    top["wheel_min"] = wheel_min;
    top["wheel_max"] = wheel_max;
    top["wheel_moved"] = wheel_moved;
    // The reports that came in, and the zero-length packets after them.
    top["reports_in"] = hid_usb_report_count();
    top["zlps"] = hid_usb_zlp_count();
    top["out_sent"] = hid_usb_out_count();
    top["out_status"] = hid_usb_last_out_status();
    top["out_no_device"] = hid_usb_out_refused(0);
    top["out_too_long"] = hid_usb_out_refused(1);
    top["out_busy"] = hid_usb_out_refused(2);
    top["out_submit_err"] = hid_usb_out_refused(3);
    top["out_submit_code"] = hid_usb_out_submit_error();
    top["out_stuck"] = hid_usb_out_stuck();
    top["ctrl_out"] = hid_usb_ctrl_out_count();
    top["out_bytes"] = hid_usb_out_bytes();
    top["out_via_control"] = hid_usb_get_out_via_control() ? 1 : 0;
    top["out_drop_leading"] = hid_usb_get_out_drop_leading() ? 1 : 0;
    top["out_zlp"] = hid_usb_get_out_zlp() ? 1 : 0;
    top["in_xfer_len"] = hid_usb_get_in_xfer_len();
    top["out_shift"] = hid_usb_get_out_shift() ? 1 : 0;
    top["out_cap"] = hid_usb_get_out_cap();
    top["out_gap"] = opendeck_out_gap;
    top["link_speed"] = hid_usb_link_speed();
    {
      uint8_t fl = 0;
      const uint8_t* fb = hid_usb_out_first(&fl);
      char fhex[2 * 32 + 1];
      int fhl = 0;
      for (uint8_t i = 0; i < fl && fhl < (int)sizeof(fhex) - 3; i++) {
        fhl += snprintf(fhex + fhl, sizeof(fhex) - fhl, "%02X", fb[i]);
      }
      top["out_first"] = fhex;
    }
    {
      // The device's own report descriptor and the head of the last
      // packet that went out: between them they say what the device
      // expects and what we sent it.
      uint16_t dl = 0;
      const uint8_t* dd = hid_usb_report_desc(&dl);
      char hex[2 * 256 + 1];
      int hl = 0;
      for (uint16_t i = 0; i < dl && hl < (int)sizeof(hex) - 3; i++) {
        hl += snprintf(hex + hl, sizeof(hex) - hl, "%02X", dd[i]);
      }
      top["report_desc"] = hex;
      uint8_t ol = 0;
      const uint8_t* oh = hid_usb_last_out_head(&ol);
      char ohex[2 * 24 + 1];
      int ohl = 0;
      for (uint8_t i = 0; i < ol && ohl < (int)sizeof(ohex) - 3; i++) {
        ohl += snprintf(ohex + ohl, sizeof(ohex) - ohl, "%02X", oh[i]);
      }
      top["last_out_head"] = ohex;
    }
    // Every report that has arrived: one entry per report ID, with the
    // latest bytes and how many have come in.
    JsonArray rs = top.createNestedArray("reports");
    for (uint8_t i = 0; i < seen_count; i++) {
      JsonObject o = rs.createNestedObject();
      o["id"] = seen[i].id;
      o["len"] = seen[i].len;
      o["count"] = seen[i].count;
      char hex[2 * HIDCTL_SEEN_BYTES + 1];
      int hl = 0;
      hex[0] = '\0';
      for (uint8_t k = 0; k < seen[i].len && k < HIDCTL_SEEN_BYTES && hl < (int)sizeof(hex) - 3; k++) {
        hl += snprintf(hex + hl, sizeof(hex) - hl, "%02X", seen[i].bytes[k]);
      }
      o["bytes"] = hex;
    }
    top["page"] = page;
    top["page_count"] = lastPage();
    // The preset that is active, tracked across the core's wiping of
    // currentPreset - this is the one the green pad shows.
    top["active_preset"] = tracked_active_preset;
    top["report_status"] = hid_usb_last_report_status();
    top["reports_ok"] = hid_usb_reports_ok();
    uint8_t rb_len = 0;
    const uint8_t* rb = hid_usb_readback(&rb_len);
    if (rb_len) {
      char hex[2 * HIDCTL_READBACK_BYTES + 1];
      int hl = 0;
      for (uint8_t i = 0; i < rb_len && hl < (int)sizeof(hex) - 3; i++) {
        hl += snprintf(hex + hl, sizeof(hex) - hl, "%02X", rb[i]);
      }
      top["readback"] = hex;
    }
    uint8_t out_len = 0;
    const uint8_t* out = hid_usb_last_out_report(&out_len);    if (out_len) {
      char hex[2 * HIDCTL_READBACK_BYTES + 1];
      int hl = 0;
      for (uint8_t i = 0; i < out_len && hl < (int)sizeof(hex) - 3; i++) {
        hl += snprintf(hex + hl, sizeof(hex) - hl, "%02X", out[i]);
      }
      top["led_report"] = hex;
    }
    JsonArray pr = top.createNestedArray("probe");
    for (uint8_t i = 0; i < probe_result_count; i++) {
      JsonObject o = pr.createNestedObject();
      o["type"] = probe_results[i].type;
      o["id"] = probe_results[i].id;
      o["len"] = probe_results[i].len;
      o["status"] = probe_results[i].status;
    }
    // The wire log: every report that differed from the one before, with
    // the packets that came in between. This is the data to crunch.
    top["log_reports"] = hid_usb_log_reports();
    top["log_zlps"] = hid_usb_log_zlps();
    top["in_errors"] = hid_usb_in_errors();
    {
      uint8_t sc = 0;
      uint16_t* lens = nullptr;
      const uint8_t* sts = hid_usb_in_statuses(&sc, &lens);
      JsonArray sa = top.createNestedArray("in_completions");
      for (uint8_t i = 0; i < sc; i++) {
        JsonObject o = sa.createNestedObject();
        o["status"] = sts[i];
        o["len"] = lens ? lens[i] : 0;
      }
    }
    {
      uint8_t lc = hid_usb_log_count();
      JsonArray la = top.createNestedArray("log");
      for (uint8_t i = 0; i < lc; i++) {
        const HidLogEntry* ep = hid_usb_log(i);
        if (!ep) continue;
        const HidLogEntry& e = *ep;
        JsonObject o = la.createNestedObject();
        char hex[2 * HIDCTL_MAX_REPORT + 1];
        int hl = 0;
        hex[0] = '\0';
        for (uint8_t k = 0; k < e.len && hl < (int)sizeof(hex) - 3; k++) {
          hl += snprintf(hex + hl, sizeof(hex) - hl, "%02X", e.data[k]);
        }
        o["d"] = hex;
        o["s"] = e.skipped;
      }
    }
    // The configuration descriptor, as the device sent it. This is the
    // authority on the device's layout - the parse above can only be as
    // good as the walk it does.
    uint8_t cfg_len = 0;
    const uint8_t* cfg = hid_usb_config_desc(&cfg_len);
    if (cfg_len) {
      char hex[2 * HIDCTL_READBACK_BYTES + 1];
      int hl = 0;
      hex[0] = '\0';
      for (uint8_t i = 0; i < cfg_len && hl < (int)sizeof(hex) - 3; i++) {
        hl += snprintf(hex + hl, sizeof(hex) - hl, "%02X", cfg[i]);
      }
      top["config_desc"] = hex;
      top["config_len"] = cfg_len;
    }
    // What the device says it has: every interface and its endpoints.
    uint8_t ifcount = 0;
    const HidctlIfaceInfo* ifs = hid_usb_interfaces(&ifcount);
    JsonArray ia = top.createNestedArray("interfaces");
    for (uint8_t i = 0; i < ifcount; i++) {
      JsonObject o = ia.createNestedObject();
      o["num"] = ifs[i].num;
      o["class"] = ifs[i].cls;
      o["subclass"] = ifs[i].sub;
      o["protocol"] = ifs[i].proto;
      o["endpoints"] = ifs[i].ep_count;
      char eps[24];
      int len = 0;
      eps[0] = '\0';
      for (uint8_t e = 0; e < ifs[i].ep_count && len < 20; e++) {
        len += snprintf(eps + len, sizeof(eps) - len, "%s0x%02X",
                        e ? "," : "", ifs[i].ep_addr[e]);
      }
      o["ep_addr"] = eps;
      char attrs[24];
      int alen = 0;
      attrs[0] = '\0';
      for (uint8_t e = 0; e < ifs[i].ep_count && alen < 20; e++) {
        alen += snprintf(attrs + alen, sizeof(attrs) - alen, "%s0x%02X",
                         e ? "," : "", ifs[i].ep_attr[e]);
      }
      o["ep_attr"] = attrs;
      char mps[24];
      int mlen = 0;
      mps[0] = '\0';
      for (uint8_t e = 0; e < ifs[i].ep_count && mlen < 20; e++) {
        mlen += snprintf(mps + mlen, sizeof(mps) - mlen, "%s%u",
                         e ? "," : "", (unsigned)ifs[i].ep_mps[e]);
      }
      o["ep_mps"] = mps;
    }
    if (active_profile < profile_count) {
      HidctlProfile& p = profiles[active_profile];
      JsonObject vals = top.createNestedObject("values");
      for (uint8_t i = 0; i < p.control_count; i++) {
        vals[p.controls[i].name] = control_value[i];
      }
    }
  }

  void addToConfig(JsonObject& obj) override {
    Usermod::addToConfig(obj);
    JsonObject top = obj[FPSTR(_name)];
    if (top.isNull()) top = obj.createNestedObject(FPSTR(_name));
    top[FPSTR(_key_enabled)] = enabled;
  }

  bool readFromConfig(JsonObject& obj) override {
    bool configComplete = Usermod::readFromConfig(obj);
    JsonObject top = obj[FPSTR(_name)];
    if (top.isNull()) return configComplete;
    configComplete &= getJsonValue(top[FPSTR(_key_enabled)], enabled);
    return configComplete;
  }

  uint16_t getId() override { return USERMOD_ID_HIDCTL; }
};

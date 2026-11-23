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
#include "hid_dwc_probe.h"
#include <LovyanGFX.hpp>
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

  // -------------------------------------------------------------------------
  // The connected surfaces
  // -------------------------------------------------------------------------
  //
  // The board has a hub, so more than one control surface can be on it at
  // once: the deck on one port and the F1 on another, or the F1 plugged
  // into the deck's own hub. Each of them is a surface: the device it is
  // on, the profile it is bound to, and the state that belongs to it.
  //
  // What the surfaces SHARE is the point of it: the page, the running
  // preset, the blinks and the picture cache are not per surface, so the
  // F1's encoder turns the page and the deck shows it.
  struct HidSurface {
    bool     in_use    = false;  // a device is on it right now
    bool     bound     = false;  // it has been given a device
    uint8_t  device    = 0;      // the host layer's device
    uint8_t  profile   = 0xFF;   // the profile it is bound to
    bool     opendeck  = false;  // it speaks the deck's command protocol
    // Its own output buffer. The F1's LED report is 81 bytes, and it
    // belongs to the F1: two surfaces must not share one.
    uint8_t  out[HIDCTL_MAX_LED] = {};
    uint16_t out_len     = 0;
    bool     out_pending = false;
    // The decoded state of THIS device.
    //
    //   control_value      the raw value that arrived
    //   control_taken      the control has crossed its parameter's value
    //   control_applied    the raw value last driven into WLED - the
    //                      baseline the deadband is measured against
    //   control_mapped     the 0..255 value that produced, so an unchanged
    //                      result is not driven again
    //   control_pending    a newer value is waiting for the throttle
    //   control_has_applied the baseline is meaningful
    //   control_waited     the takeover wait has already been said once
    //   control_applied_ms when it was last driven
    //   control_log_ms     when it was last written to the console
    uint16_t control_value[HIDCTL_MAX_CONTROLS] = {0};
    uint8_t  control_taken[HIDCTL_MAX_CONTROLS]  = {0};
    uint16_t control_applied[HIDCTL_MAX_CONTROLS]     = {0};
    uint8_t  control_mapped[HIDCTL_MAX_CONTROLS]      = {0};
    bool     control_has_applied[HIDCTL_MAX_CONTROLS] = {false};
    bool     control_pending[HIDCTL_MAX_CONTROLS]      = {false};
    bool     control_waited[HIDCTL_MAX_CONTROLS]       = {false};
    uint32_t control_applied_ms[HIDCTL_MAX_CONTROLS]   = {0};
    uint32_t control_log_ms[HIDCTL_MAX_CONTROLS]      = {0};
    uint32_t buttons = 0;
    uint32_t modifier_mask = 0;
    // The page this device is on. The page is as many slots as the device
    // has screens, so the F1's pages are sixteen presets and the deck's are
    // ten - and each device browses its own pages.
    uint8_t  page = 1;
    // When the page was last shown on the display, and until when.
    uint32_t show_page_until = 0;
    // The wheel's position belongs to the connection: a device that is
    // unplugged and plugged back in starts again from nothing.
    uint8_t  wheelLast = 0;
    bool     wheelSeen = false;
    // When it was last painted, so the paint can be throttled per surface.
    uint32_t last_paint = 0;
  };
  static constexpr uint8_t HIDCTL_MAX_SURFACES = 4;
  HidSurface surfaces[HIDCTL_MAX_SURFACES] = {};
  uint8_t surface_count = 0;
  // The surface the console and the web page talk about: the one that was
  // touched last.
  uint8_t focus_surface = 0xFF;
  // The surface whose report is being handled. It is what "the profile"
  // means while a report is being read.
  uint8_t cur_surface = 0xFF;

  // The surface on a device, or 0xFF.
  uint8_t surfaceForDevice(uint8_t device) const {
    for (uint8_t i = 0; i < surface_count; i++) {
      if (surfaces[i].in_use && surfaces[i].device == device) return i;
    }
    return 0xFF;
  }

  // The profile of the surface being handled.
  uint8_t activeProfile() const {
    if (cur_surface < surface_count && surfaces[cur_surface].in_use)
      return surfaces[cur_surface].profile;
    return 0xFF;
  }

  // The profile of the surface the console talks about.
  uint8_t focusProfile() const {
    if (focus_surface < surface_count && surfaces[focus_surface].in_use)
      return surfaces[focus_surface].profile;
    return 0xFF;
  }

  // The device the console talks about.
  uint8_t focusDevice() const {
    if (focus_surface < surface_count && surfaces[focus_surface].in_use)
      return surfaces[focus_surface].device;
    return 0;
  }

  // The deck: the surface that shows the pictures. There is one deck's
  // worth of machinery - the picture cache and the build task - so it
  // belongs to the first deck that is connected.
  uint8_t deckSurface() const {
    for (uint8_t i = 0; i < surface_count; i++) {
      if (surfaces[i].in_use && surfaces[i].opendeck) return i;
    }
    return 0xFF;
  }
  uint8_t deckProfile() const {
    const uint8_t s = deckSurface();
    return (s == 0xFF) ? 0xFF : surfaces[s].profile;
  }
  uint8_t deckDevice() const {
    const uint8_t s = deckSurface();
    return (s == 0xFF) ? 0xFF : surfaces[s].device;
  }

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
  // ...and the rest of them are in the surface as well: each device has
  // its own controls, and they must not be counted against each other.

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
  void resetControls(uint8_t s) {
    HidSurface& sf = surfaces[s];
    for (uint8_t i = 0; i < HIDCTL_MAX_CONTROLS; i++) {
      sf.control_value[i] = 0xFFFF;   // force the next report to apply
      sf.control_taken[i] = 0;
      sf.control_applied[i] = 0;
      sf.control_mapped[i] = 0;
      sf.control_has_applied[i] = false;
      sf.control_pending[i] = false;
      sf.control_waited[i] = false;
      sf.control_applied_ms[i] = 0;
      sf.control_log_ms[i] = 0;
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
  // The presets are paged in blocks: the pads address the current page,
  // and the encoder turns the page. Turning it clockwise goes forward a
  // page and anticlockwise back; it never wraps - the first page stays
  // the first and the last page stays the last.
  //
  // The page is as many slots as the device has screens: sixteen for the
  // F1's pads, ten for the CN003's keys.
  static constexpr uint8_t PAGE_SIZE = 16;

  // How many slots a page holds, for a profile.
  //
  // A page is as many slots as the device has screens, so the two devices
  // have different page sizes: the F1's pads are sixteen and the CN003's
  // keys are ten. The profile says which.
  uint8_t pageSize(uint8_t profile) const {
    if (profile >= profile_count || !profiles) return PAGE_SIZE;
    const HidctlProfile& p = profiles[profile];
    if (p.page_size) return p.page_size;
    // A deck's pages are as many slots as it has keys with screens.
    if (p.opendeck && p.od_keys) return p.od_keys;
    return PAGE_SIZE;
  }

  uint8_t  wheelLast = 0;     // the encoder's last position
  bool     wheelSeen = false;
  uint32_t showWheelUntil = 0; // the encoder's position is on the display until then

  // The last page a preset can live on, for a profile. The pages are not
  // limited to the presets that happen to be saved: the user browses
  // ahead of them and saves there.
  uint8_t lastPage(uint8_t profile) const {
    const uint8_t size = pageSize(profile);
    return (uint8_t)((250 + size - 1) / size);
  }

  // The page the current preset is in, for a profile.
  uint8_t pageOfPreset(uint16_t preset, uint8_t profile) const {
    if (preset < 1) preset = 1;
    return (uint8_t)(((preset - 1) / pageSize(profile)) + 1);
  }

  // The offset a device's pads are shifted by: page one is the first slots
  // of the page, page two the next ones, and so on. It is the device's own
  // page and its own page size.
  uint16_t pageOffset(uint8_t s) const {
    if (s >= surface_count || !surfaces[s].in_use) return 0;
    return (uint16_t)((surfaces[s].page - 1) * pageSize(surfaces[s].profile));
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
  void turnPage(uint8_t s, int direction) {
    if (s >= surface_count || !surfaces[s].in_use) return;
    const uint8_t profile = surfaces[s].profile;
    const uint8_t last = lastPage(profile);
    int want = (int)surfaces[s].page + direction;
    if (want < 1) want = 1;
    if (want > last) want = last;
    if (want == (int)surfaces[s].page) return;
    surfaces[s].page = (uint8_t)want;
    surfaces[s].show_page_until = millis() + 1500;
    USER_PRINTF("[HIDCTL] page %u\n", surfaces[s].page);
    // The page is on the display for a moment, and the pads move to the
    // new page's slots whether or not a preset is saved on them.
    paint();
    // A deck's screens show the page's slots, so they change with it.
    repaintDeck();
  }

  // Show the page on the display for a moment.
  void showPageBriefly(uint8_t s) {
    if (s >= surface_count) return;
    surfaces[s].show_page_until = millis() + 1500;
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
  // The deck's command buffer. Every command is built here and padded to
  // the wire length, and it belongs to the deck: the plain HID surfaces
  // have their own report buffers.
  uint8_t opendeck_buf[HIDCTL_MAX_OUTPUT] = {};
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
        p.od_strip_bgpic = od["bgpic"] | false;
      }
    }

    // How many slots a page holds. Zero means "as many as the device has
    // screens", so a deck pages by its keys and the F1 by its pads.
    p.page_size = (uint8_t)(j["page_size"] | 0);
    if (p.page_size > 250) p.page_size = 0;

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
      enc.touched = false;
      // An encoder whose action is one of the effect's controls cycles
      // through them: the touch zone above it picks which one.
      enc.ctrl = 0;
      enc.cycler = false;
      for (uint8_t c = 0; c < OPENDECK_CTRLS; c++) {
        if (!strcmp(enc.action, opendeckCtrlAction(c))) {
          enc.ctrl = c;
          enc.cycler = true;
          break;
        }
      }
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
    const uint8_t profile = focusProfile();
    if (profile < profile_count) return profiles[profile].name;
    return "";
  }

  // Is this profile one of the OpenDeck-family decks? They are not plain
  // HID devices: their packets are a command protocol, and the way they
  // go on the wire is different too.
  bool profileIsOpendeck(uint8_t idx) const {
    return idx < profile_count && profiles[idx].opendeck;
  }

  // The USB host client found a device. Bind the first profile that wants it.
  void onDeviceConnected(uint8_t device, uint16_t vid, uint16_t pid) {
    // Re-read the profiles: one may have been edited while the controller
    // was unplugged, and this is the moment it comes back.
    loadProfiles();
    uint8_t idx = matchDevice(vid, pid);
    if (idx == 0xFF) {
      USER_PRINTF("[HIDCTL] device %u 0x%04X:0x%04X — no profile wants it\n", (unsigned)device, vid, pid);
      return;
    }
    // The surface for this device. A device that has been here before
    // keeps the surface it had - the host layer gave it the same device
    // index it had last time - and a new one takes a free surface.
    uint8_t s = 0xFF;
    for (uint8_t i = 0; i < HIDCTL_MAX_SURFACES; i++) {
      if (surfaces[i].bound && surfaces[i].device == device) { s = i; break; }
    }
    if (s == 0xFF) {
      for (uint8_t i = 0; i < HIDCTL_MAX_SURFACES; i++) {
        if (!surfaces[i].bound) { s = i; break; }
      }
      if (s == 0xFF) {
        USER_PRINTF("[HIDCTL] no room for another surface\n");
        return;
      }
      surfaces[s].bound = true;
      surfaces[s].device = device;
    }
    surfaces[s].in_use = true;
    // The surfaces are numbered from zero and the count is how many of
    // them have ever been taken: a surface that is freed keeps its place
    // so the device that had it finds it again.
    if (s >= surface_count) surface_count = (uint8_t)(s + 1);
    surfaces[s].profile  = idx;
    surfaces[s].opendeck = profiles[idx].opendeck;
    focus_surface = s;
    resetControls(s);
    surfaces[s].buttons = 0;
    // The wheel's position belongs to the connection: forget the old one so
    // the first report after a reconnect is not read as a turn.
    surfaces[s].wheelSeen = false;
    surfaces[s].wheelLast = 0;
    // How many slots a page holds belongs to the device, so the page the
    // running preset is on has to be worked out again now that the
    // profile is known: the deck's pages are ten slots and the F1's are
    // sixteen.
    if (tracked_active_preset) {
      surfaces[s].page = pageOfPreset(tracked_active_preset, surfaces[s].profile);
    }
    USER_PRINTF("[HIDCTL] %s connected (device %u)\n", profiles[idx].name,
                (unsigned)device);
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
      hid_usb_set_out_drop_leading(device, true);
      hid_usb_set_out_shift(device, false);
      hid_usb_set_out_zlp(device, false);
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

  void onDeviceDisconnected(uint8_t device) {
    const uint8_t s = surfaceForDevice(device);
    if (s == 0xFF) return;
    const bool was_deck = surfaces[s].opendeck;
    if (surfaces[s].profile < profile_count) {
      USER_PRINTF("[HIDCTL] %s disconnected (device %u)\n",
                  profiles[surfaces[s].profile].name, (unsigned)device);
    }
    surfaces[s].in_use = false;
    surfaces[s].profile = 0xFF;
    surfaces[s].opendeck = false;
    surfaces[s].out_pending = false;
    surfaces[s].buttons = 0;
    if (focus_surface == s) focus_surface = 0xFF;
    if (cur_surface == s) cur_surface = 0xFF;
    // The next device that appears gets a session of its own, with its
    // own "DIS".
    if (was_deck) opendeck_session_open = false;
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
      showPageBriefly(cur_surface);
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
    if (!strcmp(action, "effectCustom3")) {
      // The fifth slider: the effect's own, and it is only five bits.
      Segment& seg = strip.getMainSegment();
      seg.custom3 = (uint8_t)(value > 31 ? 31 : value);
      applyValuesToSelectedSegs();
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }
    if (!strncmp(action, "effectOpt", 9) && isdigit(action[9])) {
      // One of the effect's check boxes: a press turns it on and off.
      const uint8_t n = (uint8_t)(action[9] - '1');
      if (n < 3) {
        Segment& seg = strip.getMainSegment();
        if (n == 0)      seg.check1 = !seg.check1;
        else if (n == 1) seg.check2 = !seg.check2;
        else             seg.check3 = !seg.check3;
        applyValuesToSelectedSegs();
        colorUpdated(CALL_MODE_BUTTON);
        stateUpdated(CALL_MODE_BUTTON);
        wsPushIfDue();
      }
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
      if (deckProfile() != 0xFF) {
        Opendeck::lig(opendeck_buf, (uint8_t)((uint16_t)ctrl_bri * 100 / 255));
        opendeckSend(opendeck_buf);
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

  // Is a button with this name currently held? The buttons are the
  // surface's own: two devices have two sets of them.
  bool buttonHeld(uint8_t s, const HidctlProfile& p, const char* name) {
    if (s >= surface_count) return false;
    for (uint8_t i = 0; i < p.button_count; i++) {
      if (!strcmp(p.buttons[i].name, name)) {
        return (surfaces[s].buttons & (1UL << p.buttons[i].index)) != 0;
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

  // The beat that is sounding now and the music playlist's slot.
  //
  // They are worked out here and not inside paint(), because a deck's
  // state is in its PICTURES and paint() does nothing for a deck: the
  // beat and the music playlist would never reach one.
  void refreshTrackerState() {
    static uint32_t last = 0;
    if (millis() - last < 100) return;
    last = millis();
    ArTempoFrame tf;
    currentBeat = 0;
    if (arCopyTempo(&tf) && tf.beatInBar >= 1 && tf.beatInBar <= 4) {
      currentBeat = tf.beatInBar;
    }
    music_slot = queryAutoPlaylistMusicSlot();
  }

  // Paint every surface that wants to be painted. They are painted one
  // after the other, each with its own profile and its own report.
  void paint() {
    if (!profiles) return;
    if (!paint_enabled) return;
    for (uint8_t s = 0; s < surface_count; s++) {
      if (!surfaces[s].in_use) continue;
      paintSurface(s);
    }
  }

  // Paint one surface: its own profile, its own report, its own buffer.
  void paintSurface(uint8_t s) {
    if (!profiles || surfaces[s].profile >= profile_count) return;
    HidctlProfile& p = profiles[surfaces[s].profile];
    if (!p.output.present) return;
    if (p.output.length > HIDCTL_MAX_LED) {
      // The report does not fit the buffer. It is said once: the profile is
      // at fault here, not the device.
      static bool said = false;
      if (!said) {
        said = true;
        USER_PRINTF("[HIDCTL] %s: the output report is %u bytes, and it can "
                    "only take %u\n", p.name, (unsigned)p.output.length,
                    (unsigned)HIDCTL_MAX_LED);
      }
      return;
    }

    // WLED fires a state change often; the LEDs do not need to follow every
    // one of them. A blink is the exception - it has to be repainted fast
    // enough to show the rate it was asked for.
    // Twenty updates a second while something is blinking, ten otherwise.
    // Faster than that is not visible - a blink is four hertz - and the
    // LED report is two USB transfers each, so a faster paint fills the
    // outgoing queue and the packets are dropped.
    uint32_t minGap = blinkActive() ? 50 : 100;
    if (millis() - surfaces[s].last_paint < minGap) return;
    surfaces[s].last_paint = millis();

    uint8_t report[HIDCTL_MAX_LED];
    memset(report, 0, sizeof(report));
    report[0] = p.output.report_id;

    // The page follows the preset, but only when a preset is really
    // applied - the core's PresetApplied event sets it. A page that is
    // being browsed (nothing saved on it) stays where it was put.

    refreshTrackerState();

    // --- the display: the page for a moment, then the preset number ---
    uint16_t val = (tracked_active_preset >= 1 && tracked_active_preset <= 99)
                   ? tracked_active_preset : 0;
    if (millis() < surfaces[s].show_page_until) {
      // The encoder turned the page: show the page. That is what the
      // encoder is for, so it is what the display shows.
      if (surfaces[s].page < 10) {
        // "P" and the page's number on the right-hand digit.
        showLetter(report, p, 'P');
        if (p.output.display.present && p.output.display.digit_count >= 2) {
          uint16_t base = p.output.display.digit_offset[1];
          uint8_t mask = digitMask(surfaces[s].page);
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
        showNumber(report, p, surfaces[s].page);
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
        int slot = atoi(nm + 3) + (int)pageOffset(s);
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
      sendLedReport(s, p, report);
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
        int slot = atoi(nm + 3) + (int)pageOffset(s);
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
      } else if (buttonHeld(s, p, nm)) {
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

    sendLedReport(s, p, report);
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
  void sendLedReport(uint8_t s, const HidctlProfile& p, const uint8_t* report) {
    uint16_t len = p.output.length;
    if (len > HIDCTL_MAX_LED) len = HIDCTL_MAX_LED;
    memcpy(surfaces[s].out, report, len);
    surfaces[s].out_len = len;
    surfaces[s].out_pending = true;
  }

  // Send the queued LED report. Only the main loop calls this.
  void flushLedReport() {
    if (!profiles) return;
    for (uint8_t s = 0; s < surface_count; s++) {
      if (!surfaces[s].in_use || !surfaces[s].out_pending) continue;
      surfaces[s].out_pending = false;
      if (surfaces[s].profile >= profile_count) continue;
      HidctlProfile& p = profiles[surfaces[s].profile];
      const uint8_t d = surfaces[s].device;
      if (led_out_endpoint) {
        hid_usb_queue_out(d, surfaces[s].out, surfaces[s].out_len);
        continue;
      }
      // The report ID goes in wValue, not in the data.
      if (surfaces[s].out_len > 1) {
        hid_usb_send_output_report(d, p.output.report_id, surfaces[s].out + 1,
                                   (uint16_t)(surfaces[s].out_len - 1));
      }
    }
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
    if (!profiles) return;
    const uint32_t now = millis();
    for (uint8_t s = 0; s < surface_count; s++) {
      if (!surfaces[s].in_use || surfaces[s].profile >= profile_count) continue;
      HidctlProfile& p = profiles[surfaces[s].profile];
      for (uint8_t i = 0; i < p.control_count; i++) {
        if (!surfaces[s].control_pending[i]) continue;
        if (surfaces[s].control_has_applied[i] &&
            (now - surfaces[s].control_applied_ms[i]) < CONTROL_APPLY_INTERVAL_MS) continue;
        applyControl(s, p, i, surfaces[s].control_value[i], now);
        surfaces[s].control_pending[i] = false;
      }
    }
  }

  // Drive one control into WLED. The soft takeover, the "did the result
  // actually change" test and the throttle all happen here.
  void applyControl(uint8_t s, HidctlProfile& p, uint8_t idx, uint16_t raw, uint32_t now) {
    HidSurface& sf = surfaces[s];
    HidctlControl& ctl = p.controls[idx];
    uint32_t full = (ctl.bits >= 16) ? 0xFFFFu : ((1u << ctl.bits) - 1);
    uint8_t value = (uint8_t)((uint32_t)raw * 255u / full);

    // A modifier turns the control into something else: SHIFT + knob4 is
    // the controller's own brightness, not the palette.
    const char* act = ctl.action;
    if (ctl.mod_action[0] && (sf.buttons & sf.modifier_mask) != 0) act = ctl.mod_action;
    if (!act[0]) return;

    // Soft takeover: the control has to cross the parameter's current
    // value before it starts driving it, so a knob that sits somewhere
    // else does not yank the value when it is first touched.
    if (!sf.control_taken[idx]) {
      int cur = currentValueOf(act);
      if (cur >= 0) {
        int curRaw = (int)((uint32_t)cur * full / 255u);
        int delta = (int)raw - curRaw;
        if (delta < 0) delta = -delta;
        int wiggle = (int)(full / 255u * 8u) + 1;   // ~8/255 of the range
        if (delta > wiggle) {
          // Say it once, not once a report.
          if (!sf.control_waited[idx]) {
            USER_PRINTF("[HIDCTL] %s: waiting for takeover (raw %u, target %u)\n",
                        ctl.name, raw, (unsigned)cur);
            sf.control_waited[idx] = true;
          }
          return;    // not driven: the baseline stays where it was
        }
      }
      sf.control_taken[idx] = 1;
      sf.control_waited[idx] = false;
    }

    // The result did not change, so there is nothing to drive. The baseline
    // still follows the knob, so its resting point is known.
    if (sf.control_has_applied[idx] && value == sf.control_mapped[idx]) {
      sf.control_applied[idx] = raw;
      sf.control_applied_ms[idx] = now;
      return;
    }
    sf.control_mapped[idx] = value;
    sf.control_applied[idx] = raw;
    sf.control_applied_ms[idx] = now;
    sf.control_has_applied[idx] = true;

    if (!act[0]) return;
    if (now - sf.control_log_ms[idx] >= CONTROL_LOG_INTERVAL_MS) {
      sf.control_log_ms[idx] = now;
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
        if (act && !strcmp(act, "nextctrl")) {
          // The touch zone cycles the control of the encoder below it.
          opendeckCycleControl((uint8_t)idx);
          break;
        }
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
        if (enc.cycler && opendeckCtrlIsCheck(enc.ctrl)) {
          // The encoder is on one of the effect's check boxes, and a press
          // turns it on and off.
          const char* act = opendeckCtrlAction(enc.ctrl);
          USER_PRINTF("[HIDCTL] %s push -> %s\n", enc.name, act);
          opendeck_strip_change = true;
          runAction(act, 255);
          opendeck_strip_change = false;
          paint();
          break;
        }
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
        // An encoder that cycles through the effect's controls drives the
        // one it is on, not the one the profile named.
        const char* act = enc.cycler ? opendeckCtrlAction(enc.ctrl) : enc.action;
        if (!act || !act[0]) break;
        if (enc.cycler && opendeckCtrlIsCheck(enc.ctrl)) {
          // A check box is turned on and off with a press, not a turn.
          break;
        }
        if (!strcmp(act, "page")) {
          // The encoder browses the pages of slots. It must not change
          // the preset itself - that is the keys' job.
          turnPage(deckSurface(), ev.delta);
          break;
        }
        // A twist is plus or minus one, so the value it drives is the one
        // it drove last, moved by one.
        //
        // The first turn starts from where the parameter is, so it nudges
        // it rather than throwing it to the bottom of its range: an
        // encoder that drove the brightness from zero would put the
        // lights out.
        // The value is the control's OWN value, moved by one, read every
        // time. Two encoders on the same control then stay in step: one
        // of them moves it and the other picks it up where it is.
        const int cur = currentValueOf(act);
        int v = (cur >= 0 ? cur : (int)enc.value) + ev.delta;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        enc.value = (uint8_t)v;
        USER_PRINTF("[HIDCTL] %s twist -> %s = %u\n", enc.name, act, v);
        // The parameter moved: the strip shows what it is at, and the
        // keys do not change. The state change that follows is told, so
        // it does not send the ten keys again.
        opendeck_strip_change = true;
        runAction(act, v);
        opendeck_strip_change = false;
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
      uint16_t slot = (uint16_t)(atoi(act + 6) + pageOffset(cur_surface));
      snprintf(buf, buflen, "preset%u", slot);
      return buf;
    }
    return act;
  }

  // The profile's button with that index, or null.
  const HidctlButton* buttonByIndex(uint8_t index) {
    const uint8_t profile = activeProfile();
    if (profile >= profile_count || !profiles) return nullptr;
    HidctlProfile& p = profiles[profile];
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
    memset(opendeck_buf, 0, Opendeck::WIRE_LEN);
    memcpy(opendeck_buf, opendeck_cmd[opendeck_cmd_pos], opendeck_cmd_len[opendeck_cmd_pos]);
    if (!opendeckSend(opendeck_buf)) return false;
    opendeck_cmd_pos++;
    if (opendeck_cmd_pos >= opendeck_cmd_count) {
      opendeck_cmd_count = 0;
      opendeck_cmd_pos = 0;
    }
    return true;
  }

  bool opendeckCmdPending() { return opendeck_cmd_pos < opendeck_cmd_count; }

  // Open the deck's session: DIS resets the display, LIG sets the
  // backlight, CLE clears every key and STP commits.
  //
  // "DIS" goes ONCE per connection. The device's display controller does
  // not survive a second one: it wedges until the deck is unplugged, and
  // then nothing shows on the deck at all. Two paths ask for the session
  // to be opened - the USB host's "a device appeared" and the core's
  // "a device changed" - and they do not always arrive together, so the
  // session is guarded rather than assumed.
  void opendeckInit() {
    if (opendeck_session_open) return;
    opendeck_session_open = true;
    // "DIS" resets the deck's display, so every picture on it is gone and
    // the cache has to be forgotten.
    opendeckClearCache();
    // The builders put the command at the front of the buffer and pad it;
    // only the first bytes matter here, so that is what is queued.
    Opendeck::dis(opendeck_buf);
    opendeckQueue(opendeck_buf, 9);
    Opendeck::lig(opendeck_buf, 50);
    opendeckQueue(opendeck_buf, 12);
    Opendeck::cle(opendeck_buf, Opendeck::CLEAR_ALL);
    opendeckQueue(opendeck_buf, 13);
    Opendeck::stp(opendeck_buf);
    opendeckQueue(opendeck_buf, 9);
    opendeck_last_keepalive = millis();
  }

  // True once the session has been opened for the device that is plugged
  // in now.
  bool opendeck_session_open = false;

  void opendeckKeepalive() {
    if (millis() - opendeck_last_keepalive < 9000) return;
    opendeck_last_keepalive = millis();
    Opendeck::connect(opendeck_buf);
    opendeckQueue(opendeck_buf, 13);
  }

  uint32_t opendeck_last_keepalive = 0;

  // Push the deck's pictures again, without opening a new session: the
  // reference sends its DIS once per connection and the device does not
  // like it repeated.
  //
  // The strip on its own is a separate repaint: it shows what the
  // encoders are doing, and a parameter that moved changes the strip and
  // nothing else. The keys show the presets, and sending them again for
  // every turn of an encoder would put a second of pictures between the
  // turn and the display.
  void repaintDeck(bool stripOnly = false) {
    if (deckProfile() == 0xFF) return;
    if (stripOnly) opendeck_pending_strip = true;
    else opendeck_pending_paint = true;
  }
  // The deck's USB work belongs to the main loop, like everything else:
  // the event bus runs its handlers in the PUBLISHER's task, and submitting
  // a transfer from there is how the USB stack got corrupted before.
  bool opendeck_pending_init = false;
  bool opendeck_pending_paint = false;
  // A repaint of the strip alone: the keys show the presets and a
  // parameter that moved does not change them.
  bool opendeck_pending_strip = false;
  // True while something has changed that only the strip shows: a
  // parameter the encoder drove, or the effect, whose own names for its
  // controls are on the strip. The state change that follows is not a
  // preset changing, so the keys do not have to go out again.
  bool opendeck_strip_change = false;
  // When the strip is showing the page number instead of the encoders,
  // and when that is over. The pictures have to go out again then, or
  // the strip would keep showing the page.
  uint32_t opendeck_page_until = 0;

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
  // The PSRAM's cache line is 128 bytes, and the driver checks the
  // address and the length of a sync against it.
  static constexpr size_t OPENDECK_CACHE_LINE = 128;

  static uint8_t* opendeckAlloc(size_t size) {
    const size_t aligned = (size + (OPENDECK_CACHE_LINE - 1)) & ~(size_t)(OPENDECK_CACHE_LINE - 1);
    uint8_t* p = (uint8_t*)heap_caps_aligned_alloc(OPENDECK_CACHE_LINE, aligned,
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!p) p = (uint8_t*)heap_caps_aligned_alloc(OPENDECK_CACHE_LINE, aligned,
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
    const size_t pic_sync = (((size_t)w * h * 3) + 127u) & ~(size_t)127u;
    esp_cache_msync((void*)(uintptr_t)bgr, pic_sync,
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
    // the JPEG, which is not a whole number of cache lines: the sync is
    // told about the whole line it ends in, which the buffer has room
    // for. The "UNALIGNED" flag does not stop the driver from complaining
    // about the length, so the length is rounded up instead.
    const size_t jpeg_sync = ((size_t)outSize + 127u) & ~(size_t)127u;
    esp_cache_msync((void*)(uintptr_t)jpeg, jpeg_sync,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE
                    | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
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
    const uint8_t d = deckDevice();
    if (d == 0xFF) return false;         // there is no deck
    bool ok = hid_usb_queue_out(d, built, Opendeck::WIRE_LEN);
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
  // The ten keys, the four encoder tiles and the picture across the whole
  // strip. The array has to hold all of them: an entry past the end of it
  // is a write into whatever follows it in memory, which is how the board
  // ended up crashing in a task that had nothing to do with the deck.
  static constexpr uint8_t OPENDECK_PICS = OPENDECK_KEYS + OPENDECK_ZONES + 1;

  struct OpendeckPic {
    uint8_t  slot;        // the BAT slot
    uint8_t  colour[3];   // blue, green, red
    uint16_t w, h;        // the picture's size
    uint16_t number;      // the number to draw on it, 0 = none
    uint16_t value;       // the value under it, 0 = none
    bool     has_value;   // the value is a number, not a word
    // The preset's title, which is what a key shows rather than its
    // number - the number is only for a slot with no title.
    char     title[33];
    // What an encoder's tile says it does: "PAGE", "SPEED", "BRI".
    char     label[20];
    // True when the value under the name is a check box: it says ON and
    // OFF and not a number.
    bool     is_check;
    // True when this is the key that is running. It gets a white outline
    // instead of flashing: a flashing key has to be sent again and
    // again, and it hides the title while it blinks.
    bool     is_active;
    // The hash of what this picture is drawn from. The picture is only
    // built and sent again when this changes.
    uint32_t sig;
  };
  OpendeckPic opendeck_pics[OPENDECK_PICS] = {};

  // The pictures that are on the deck, one per BAT slot.
  //
  // Every picture is a JPEG and encoding one takes long enough that the
  // pictures are built on core 1. Sending them all again every time
  // anything changed is what made the deck build pictures for ever: the
  // playlist chaser blinks a key, the blink sent all fourteen pictures
  // again, and the next blink came before the last one was away.
  //
  // So each slot keeps its picture, and a picture is built and sent again
  // only when what it shows has changed.
  static constexpr uint8_t OPENDECK_SLOTS = 16;   // the BAT slots
  uint8_t*  opendeck_cache[OPENDECK_SLOTS] = {};
  uint16_t  opendeck_cache_len[OPENDECK_SLOTS] = {};
  uint32_t  opendeck_cache_sig[OPENDECK_SLOTS] = {};
  bool      opendeck_cache_valid[OPENDECK_SLOTS] = {};

  // Forget every picture: the deck's display has been reset, or a full
  // push was asked for.
  void opendeckClearCache() {
    for (uint8_t i = 0; i < OPENDECK_SLOTS; i++) {
      if (opendeck_cache[i]) free(opendeck_cache[i]);
      opendeck_cache[i] = nullptr;
      opendeck_cache_len[i] = 0;
      opendeck_cache_sig[i] = 0;
      opendeck_cache_valid[i] = false;
    }
  }

  // The hash of everything a picture is drawn from: the colour, the
  // number, the value and the two texts.
  static uint32_t opendeckPicSignature(const OpendeckPic& pic) {
    uint32_t sig = 2166136261u;
    for (uint8_t i = 0; i < 3; i++) { sig ^= pic.colour[i]; sig *= 16777619u; }
    sig ^= pic.number;              sig *= 16777619u;
    sig ^= pic.value;               sig *= 16777619u;
    sig ^= pic.has_value ? 1 : 0;   sig *= 16777619u;
    sig ^= pic.is_check ? 1 : 0;    sig *= 16777619u;
    sig ^= pic.is_active ? 1 : 0;   sig *= 16777619u;
    for (const char* t = pic.title; *t; t++) { sig ^= (uint8_t)*t; sig *= 16777619u; }
    for (const char* t = pic.label; *t; t++) { sig ^= (uint8_t)*t; sig *= 16777619u; }
    return sig;
  }

  uint8_t opendeck_queue_count = 0;
  uint8_t opendeck_queue_pos = 0;
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
  //
  // The keys can be left out: they show the presets, and a parameter
  // that moved does not change them. Sending them again would put a
  // second of pictures between the turn of an encoder and the display.
  void opendeckBuildQueue(bool withKeys = true) {
    opendeck_queue_count = 0;
    const uint8_t deck_profile = deckProfile();
    const uint8_t keys =
        (deck_profile < profile_count && profiles[deck_profile].od_keys)
            ? profiles[deck_profile].od_keys : OPENDECK_KEYS;
    for (uint8_t i = 0; withKeys && i < keys && i < OPENDECK_KEYS; i++) {
      if (opendeck_queue_count >= OPENDECK_PICS) break;
      const uint8_t slot = opendeckSlotFor(i);
      if (!slot) continue;
      // The key shows the same preset the pad of the F1 would: the one at
      // this key's place in the CURRENT PAGE. Using the key's own number
      // instead would mean the active preset could never be highlighted
      // when it lives past the first ten.
      const int slot_no = (int)pageOffset(deckSurface()) + (int)i + 1;
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
      // A key that is running is marked with a white outline around it,
      // and NOT by flashing: a flashing key has to be sent again and
      // again, and while it blinks it is black and the title cannot be
      // read. The outline says the same thing and the picture only has
      // to go out again when the running key changes.
      const bool is_active = (slot_no == (int)tracked_active_preset);
      OpendeckPic& pic = opendeck_pics[opendeck_queue_count];
      pic.slot = slot;
      pic.colour[0] = b; pic.colour[1] = g; pic.colour[2] = r;
      pic.w = Opendeck::KEY_W; pic.h = Opendeck::KEY_H;
      // The number on the key is the slot it holds, so the deck reads as
      // a bank of presets.
      pic.number = (uint16_t)slot_no;
      // Everything about the picture is said again, every time: the keys
      // and the strip's tiles share one array, and a tile that is left
      // out of the queue makes the next one land on its place.
      pic.label[0] = 0;
      pic.has_value = false;
      pic.is_check = false;
      pic.is_active = is_active;
      // And the title of the preset it holds, which is what the key shows
      // in place of the number - a number only says which slot it is, a
      // title says what it does. The number stays in the corner.
      //
      // It comes from the core's preset cache, so no file is read and no
      // memory is given away: the pictures are pushed again and again.
      pic.title[0] = 0;
      if (slot_no >= 1 && slot_no <= 250 && presetCache != nullptr
          && presetCache[slot_no].exists) {
        strncpy(pic.title, presetCache[slot_no].name, sizeof(pic.title) - 1);
        pic.title[sizeof(pic.title) - 1] = 0;
      }
      // The picture is only worth sending again if what it shows has
      // changed. The deck keeps every picture, so a key that did not
      // change is left alone.
      pic.sig = opendeckPicSignature(pic);
      if (opendeck_cache_valid[pic.slot] && opendeck_cache_sig[pic.slot] == pic.sig) continue;
      // Most of the device's key pictures are read upside down. The
      // bottom row is the other way round, so those are sent as they are.
      opendeck_queue_count++;
    }
    // The wide touch strip.
    //
    // Four zone pictures, one per encoder, and the deck puts them side
    // by side on its one screen. That is how the reference drives this
    // family, and it is the only way the CN003 takes: with the one
    // picture across the whole strip ("BGPIC") and nothing else, the
    // strip stayed black.
    //
    // The one-picture command is still there for a deck that takes it,
    // and the profile says which: "bgpic" in the opendeck object.
    const bool bgpic = (deck_profile < profile_count && profiles)
                       && profiles[deck_profile].od_strip_bgpic;
    {
      for (uint8_t i = 0; i < OPENDECK_ZONES; i++) {
        if (opendeck_queue_count >= OPENDECK_PICS) break;
        const uint8_t hue = i * 36;
        OpendeckPic& pic = opendeck_pics[opendeck_queue_count];
        pic.slot = (uint8_t)(i + 1);            // the strip zones are slots 1..4
        pic.colour[0] = (uint8_t)(127 * (1 + 0.9 * __builtin_cosf(3.14159f * (hue + 240) / 180)));
        pic.colour[1] = (uint8_t)(127 * (1 + 0.9 * __builtin_cosf(3.14159f * (hue + 120) / 180)));
        pic.colour[2] = (uint8_t)(127 * (1 + 0.9 * __builtin_cosf(3.14159f * hue / 180)));
        pic.w = Opendeck::DIAL_W; pic.h = Opendeck::DIAL_H;
        pic.number = (uint16_t)(i + 1);         // the encoder's number
        pic.title[0] = 0;
        pic.label[0] = 0;
        pic.has_value = false;
        pic.is_check = false;
        // And what it is doing: the action it drives, in a word the strip
        // can show, and the value that action is at. The strip is the
        // deck's readout and each of its tiles belongs to one of the
        // encoders below it, so the tile says what that encoder DOES.
        if (deck_profile < profile_count && profiles &&
            i < profiles[deck_profile].od_encoders &&
            profiles[deck_profile].encoders[i].action[0]) {
          const HidctlEncoder& enc = profiles[deck_profile].encoders[i];
          if (enc.cycler) {
            // The encoder drives one of the effect's controls, and the
            // effect names it. An effect that names its controls but
            // does not use this one leaves it nameless, and the tile is
            // blacked out. An effect that names nothing at all just
            // calls its sliders Speed and Intensity.
            char label[20] = "";
            bool named = true;
            if (opendeckEffectHasNames()) {
              named = opendeckEffectName(enc.ctrl, label, sizeof(label));
            } else {
              opendeckActionLabel(opendeckCtrlAction(enc.ctrl), label, sizeof(label));
            }
            if (named) {
              strncpy(pic.label, label, sizeof(pic.label) - 1);
              pic.label[sizeof(pic.label) - 1] = 0;
              pic.is_check = opendeckCtrlIsCheck(enc.ctrl);
              const int cur = opendeckCtrlValue(enc.ctrl);
              if (cur >= 0) {
                pic.value = (uint16_t)cur;
                pic.has_value = true;
              }
            } else {
              pic.label[0] = 0;
              pic.colour[0] = pic.colour[1] = pic.colour[2] = 0;
            }
          } else {
            const char* act = enc.action;
            opendeckActionLabel(act, pic.label, sizeof(pic.label));
            if (!strcmp(act, "page")) {
              pic.value = surfaces[deckSurface() < surface_count ? deckSurface() : 0].page;
              pic.has_value = true;
            } else {
              const int cur = currentValueOf(act);
              if (cur >= 0) {
                pic.value = (uint16_t)cur;
                pic.has_value = true;
              }
            }
          }
        }
        // Only send it again if what it shows has changed.
        pic.sig = opendeckPicSignature(pic);
        if (opendeck_cache_valid[pic.slot] && opendeck_cache_sig[pic.slot] == pic.sig) continue;
        opendeck_queue_count++;
      }
    }
    if (bgpic && opendeck_queue_count < OPENDECK_PICS) {
      OpendeckPic& pic = opendeck_pics[opendeck_queue_count];
      pic.slot = 0;                          // slot 0 is the whole strip
      pic.colour[0] = pic.colour[1] = pic.colour[2] = 0;
      pic.w = Opendeck::STRIP_W; pic.h = Opendeck::STRIP_H;
      pic.number = 0;
      pic.title[0] = 0;
      pic.label[0] = 0;
      pic.has_value = false;
      pic.sig = opendeckPicSignature(pic);
      if (opendeck_cache_valid[pic.slot] && opendeck_cache_sig[pic.slot] == pic.sig) {
        opendeck_queue_count--;
      } else {
        opendeck_queue_count++;
      }
    }
    opendeck_queue_pos = 0;
    opendeck_chunk_off = 0;
    opendeck_stage = OPENDECK_STAGE_ENCODE;
  }

  // The pictures are drawn with the same library the display uses, so the
  // deck's screens carry the same font as the display - and the sprite's
  // memory is in PSRAM, because the internal heap on this board is nearly
  // full.
  LGFX_Sprite opendeckSprite;
  bool opendeckSpriteReady = false;

  // The sprite's own memory.
  //
  // LovyanGFX asks the Arduino heap for the sprite's buffer, and on this
  // board that is the internal heap - which is nearly full, and the wide
  // strip's picture does not fit in it at all. The buffer is ours and it is
  // in PSRAM. One buffer, big enough for the widest picture, given to the
  // sprite for each picture.
  uint8_t* opendeckSpriteBuf = nullptr;
  size_t opendeckSpriteBufLen = 0;

  bool opendeckSpriteBegin(uint16_t w, uint16_t h) {
    const size_t need = (size_t)w * h * 4 + 64;
    if (need > opendeckSpriteBufLen) {
      if (opendeckSpriteBuf) free(opendeckSpriteBuf);
      opendeckSpriteBuf = (uint8_t*)heap_caps_aligned_alloc(64, need,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      opendeckSpriteBufLen = opendeckSpriteBuf ? need : 0;
      if (!opendeckSpriteBuf) {
        USER_PRINTLN("[HIDCTL] no room in PSRAM for the picture");
        return false;
      }
    }
    if (opendeckSprite.width() != w || opendeckSprite.height() != h ||
        !opendeckSpriteReady) {
      opendeckSprite.setColorDepth(32);
      opendeckSprite.setBuffer(opendeckSpriteBuf, w, h, 32);
      opendeckSprite.setFont(&fonts::DejaVu72);
      opendeckSpriteReady = true;
    }
    if (!opendeckSprite.width()) {
      USER_PRINTLN("[HIDCTL] the picture sprite would not start");
      return false;
    }
    return true;
  }

  // Fill a rectangle of the sprite with a colour and put a number in the
  // middle of it. The colour is in the encoder's order: blue first.
  //
  // With a second text, the number goes in the top half and the second
  // text under it: the wide strip's tiles show which encoder they are
  // and what it is doing.
  void opendeckSpriteTile(uint16_t x0, uint16_t y0, uint16_t rw, uint16_t rh,
                          const char* text, uint8_t blue, uint8_t green, uint8_t red,
                          const char* sub = nullptr) {
    opendeckSprite.fillRect(x0, y0, rw, rh, lgfx::color565(red, green, blue));
    opendeckSprite.setTextColor(0xFFFF);
    if (sub && sub[0]) {
      opendeckSprite.setFont(&fonts::DejaVu72);
      const int tw = opendeckSprite.textWidth(text ? text : "");
      const int th = opendeckSprite.fontHeight();
      opendeckSprite.drawString(text ? text : "", x0 + (int)((rw > tw) ? (rw - tw) / 2 : 0),
                                          y0 + (int)((rh / 2 > th) ? (rh / 2 - th) / 2 : 0));
      opendeckSprite.setFont(&fonts::DejaVu40);
      const int sw = opendeckSprite.textWidth(sub);
      const int sh = opendeckSprite.fontHeight();
      opendeckSprite.drawString(sub, x0 + (int)((rw > sw) ? (rw - sw) / 2 : 0),
                                      y0 + rh / 2 + (int)((rh / 2 > sh) ? (rh / 2 - sh) / 2 : 0));
      return;
    }
    if (!text || !text[0]) return;
    opendeckSprite.setFont(&fonts::DejaVu72);
    const int tw = opendeckSprite.textWidth(text);
    const int th = opendeckSprite.fontHeight();
    opendeckSprite.drawString(text, x0 + (int)((rw > tw) ? (rw - tw) / 2 : 0),
                                    y0 + (int)((rh > th) ? (rh - th) / 2 : 0));
  }

  // A white outline around a picture. It says the key that is running
  // without the picture having to be sent again to flash it.
  void opendeckSpriteOutline(uint16_t w, uint16_t h, uint8_t thickness) {
    const uint16_t white = lgfx::color565(255, 255, 255);
    for (uint8_t i = 0; i < thickness; i++) {
      opendeckSprite.drawRect(i, i, (int32_t)w - 1 - 2 * i, (int32_t)h - 1 - 2 * i, white);
    }
  }

  // The picture, in the order the encoder reads it: the first byte of a
  // pixel is the blue one.
  void opendeckSpriteToBuffer(uint8_t* bgr, uint16_t w, uint16_t h) {
    opendeckSprite.readRectRGB(0, 0, w, h, bgr);
  }

  // The effect's controls, in the order an encoder cycles through them:
  // the five sliders and then the three check boxes.
  //
  // The effect names them in the same order - its data string is
  // "<five slider names>,<three check box names>" - so the n-th name is
  // the n-th control.
  static constexpr uint8_t OPENDECK_CTRLS = 8;
  static const char* opendeckCtrlAction(uint8_t ctrl) {
    static const char* const acts[OPENDECK_CTRLS] = {
      "effectSpeed", "effectIntensity", "effectCustom1", "effectCustom2",
      "effectCustom3", "effectOpt1", "effectOpt2", "effectOpt3"
    };
    return (ctrl < OPENDECK_CTRLS) ? acts[ctrl] : "";
  }
  static bool opendeckCtrlIsCheck(uint8_t ctrl) {
    return ctrl >= 5 && ctrl < OPENDECK_CTRLS;
  }

  // The value of one of the effect's controls, for the strip.
  int opendeckCtrlValue(uint8_t ctrl) {
    if (opendeckCtrlIsCheck(ctrl)) {
      Segment& seg = strip.getMainSegment();
      switch (ctrl) {
        case 5:  return seg.check1 ? 1 : 0;
        case 6:  return seg.check2 ? 1 : 0;
        default: return seg.check3 ? 1 : 0;
      }
    }
    return currentValueOf(opendeckCtrlAction(ctrl));
  }

  // The touch zones are the four buttons above the encoders: the strip's
  // four tiles are one per encoder, so a touch on one of them belongs to
  // the encoder below it.
  static constexpr uint8_t OPENDECK_ZONE_FIRST = 12;

  // Cycle the control an encoder drives. Which encoder is the touch zone
  // that was touched.
  void opendeckCycleControl(uint8_t button_index) {
    if (button_index < OPENDECK_ZONE_FIRST) return;
    const uint8_t profile = deckProfile();
    if (profile >= profile_count || !profiles) return;
    HidctlProfile& p = profiles[profile];
    const uint8_t enc_index = (uint8_t)(button_index - OPENDECK_ZONE_FIRST);
    if (enc_index >= (p.od_encoders ? p.od_encoders : 4)) return;
    HidctlEncoder& enc = p.encoders[enc_index];
    if (!enc.cycler) return;
    enc.ctrl = (uint8_t)((enc.ctrl + 1) % OPENDECK_CTRLS);
    // The new control starts from its own value.
    enc.touched = false;
    USER_PRINTF("[HIDCTL] %s -> %s\n", enc.name, opendeckCtrlAction(enc.ctrl));
    // Only the strip says what the encoder is doing now.
    repaintDeck(true);
  }

  // ---- the title on a key -------------------------------------------------
  //
  // A key shows the TITLE of the preset it holds, not just its number: a
  // number says which slot it is, a title says what it does.
  //
  // The rule for putting a title into a small box is the panel's (the
  // "WaveShare 10.1 Panel" branch does the same for its buttons): the
  // words are wrapped over as many lines as fit, a word too long for a
  // line of its own is cut short with an ellipsis, and a title that is
  // too tall for the box is cut short at the first line.
  static constexpr uint8_t OPENDECK_TEXT_LINES = 4;
  static constexpr uint8_t OPENDECK_TEXT_CHARS = 40;

  // The short name an action is shown by.
  //
  // The wide strip is the deck's readout: each of its tiles belongs to
  // one of the encoders below it, so the tile says what that encoder
  // DOES - "PAGE", "SPEED", "BRI" - and not what the action is called in
  // the profile. The strip is small, so the names are short.
  //
  // An effect decides what its own sliders are called. "Beat Sweep"
  // calls them "Cycle", "Sensitivity", "Width", "Hue" and "Effect", and
  // its check boxes are "Trail", "Loop" and "Beat" - so a strip that
  // said "SPEED" would be wrong. The names come out of the effect's own
  // data string.
  static void opendeckActionLabel(const char* action, char* out, size_t out_len) {
    if (!out || !out_len) return;
    out[0] = 0;
    if (!action || !action[0]) return;
    const char* label = action;
    if (!strcmp(action, "page")) label = "PAGE";
    else if (!strcmp(action, "bri")) label = "BRI";
    else if (!strcmp(action, "effectSpeed")) {
      // The effect's own name for its speed slider.
      if (opendeckEffectName(0, out, out_len)) return;
      label = "SPEED";
    } else if (!strcmp(action, "effectIntensity")) {
      if (opendeckEffectName(1, out, out_len)) return;
      label = "INTEN";
    } else if (!strcmp(action, "effectCustom1")) {
      if (opendeckEffectName(2, out, out_len)) return;
      label = "CUSTOM1";
    } else if (!strcmp(action, "effectCustom2")) {
      if (opendeckEffectName(3, out, out_len)) return;
      label = "CUSTOM2";
    } else if (!strcmp(action, "effectPalette")) label = "PALETTE";
    else if (!strncmp(action, "seg", 3) && isdigit(action[3])
             && !strcmp(action + 4, "bri")) {
      // seg0bri .. seg7bri: the segment's number is the name.
      snprintf(out, out_len, "SEG%c", action[3]);
      return;
    }
    strncpy(out, label, out_len - 1);
    out[out_len - 1] = 0;
  }

  // The names the running effect gives its own controls.
  //
  // The effect's data string is "Name@<names>;...". The first five
  // names are the sliders - speed, intensity and the three customs - and
  // the ones after them are the effect's check boxes. So "Beat Sweep" is
  // "Cycle,Sensitivity,Width,Hue,Effect" and then "Trail,Loop,Beat".
  //
  // The names are read from the effect's own data, so they follow the
  // effect: the same encoder says something different when a different
  // effect is running.
  static const char* opendeckEffectNames() {
    static char names[160];
    names[0] = 0;
    const uint8_t fx = strip.getMainSegment().mode;
    if (!fx) return names;
    char buf[192];
    strncpy_P(buf, strip.getModeData(fx), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char* at = strchr(buf, '@');
    if (!at) return names;
    char* end = strchr(at + 1, ';');
    if (end) *end = 0;
    strncpy(names, at + 1, sizeof(names) - 1);
    names[sizeof(names) - 1] = 0;
    return names;
  }

  // Does the running effect name its controls at all? Most do not, and
  // they call their sliders Speed and Intensity like every effect does.
  static bool opendeckEffectHasNames() {
    return opendeckEffectNames()[0] != 0;
  }

  // The name the running effect gives its n-th control: the sliders
  // first, then the check boxes. False when it has no name for it.
  static bool opendeckEffectName(uint8_t index, char* out, size_t out_len) {
    if (!out || !out_len) return false;
    out[0] = 0;
    const char* p = opendeckEffectNames();
    for (uint8_t i = 0; i <= index; i++) {
      if (!*p) return false;
      const char* comma = strchr(p, ',');
      if (i == index) {
        size_t n = comma ? (size_t)(comma - p) : strlen(p);
        if (!n) return false;
        if (n > out_len - 1) n = out_len - 1;
        memcpy(out, p, n);
        out[n] = 0;
        return true;
      }
      if (!comma) return false;
      p = comma + 1;
    }
    return false;
  }

  // The lines a text takes in a box of that width. The lines are cut
  // short with an ellipsis when they do not fit.
  uint8_t opendeckTextWrap(const char* text, char lines[][OPENDECK_TEXT_CHARS],
                           uint8_t max_lines, uint16_t rw) {
    if (!text || !text[0]) return 0;
    const int ellipsis_w = opendeckSprite.textWidth("...");
    uint8_t count = 0;
    size_t len = 0;
    const char* p = text;
    while (*p && count < max_lines) {
      // The word at p, up to the next space.
      while (*p == ' ') p++;
      if (!*p) break;
      const char* w = p;
      while (*p && *p != ' ') p++;
      size_t wl = (size_t)(p - w);

      // The line with this word on it.
      char probe[OPENDECK_TEXT_CHARS];
      size_t pl = len;
      if (pl) probe[pl++] = ' ';
      size_t n = (pl + wl > OPENDECK_TEXT_CHARS - 1) ? (OPENDECK_TEXT_CHARS - 1 - pl) : wl;
      memcpy(probe + pl, w, n);
      probe[pl + n] = 0;

      if (pl && opendeckSprite.textWidth(probe) > rw) {
        // The line is full: it ends here, and the word starts the next.
        count++;
        len = 0;
        if (count >= max_lines) break;
        n = (wl > OPENDECK_TEXT_CHARS - 1) ? (OPENDECK_TEXT_CHARS - 1) : wl;
        memcpy(lines[count], w, n);
        len = n;
        lines[count][len] = 0;
      } else {
        if (len) lines[count][len++] = ' ';
        memcpy(lines[count] + len, w, n);
        len += n;
        lines[count][len] = 0;
      }
    }
    if (len && count < max_lines) count++;

    // A line that is still too wide - one long word - is cut short.
    for (uint8_t l = 0; l < count; l++) {
      if (opendeckSprite.textWidth(lines[l]) <= rw) continue;
      size_t cut = 0;
      for (size_t k = 1; k <= strlen(lines[l]); k++) {
        char t[OPENDECK_TEXT_CHARS];
        memcpy(t, lines[l], k);
        t[k] = 0;
        if (opendeckSprite.textWidth(t) + ellipsis_w > rw) break;
        cut = k;
      }
      memcpy(lines[l] + cut, "...", 4);
    }
    return count;
  }

  // Draw a title in a box: wrapped over as many lines as fit, centred.
  void opendeckSpriteTitle(uint16_t x0, uint16_t y0, uint16_t rw, uint16_t rh,
                           const char* title) {
    char lines[OPENDECK_TEXT_LINES][OPENDECK_TEXT_CHARS];
    const int lh = opendeckSprite.fontHeight();
    if (lh <= 0) return;
    uint8_t max_lines = (uint8_t)(rh / (uint32_t)lh);
    if (max_lines > OPENDECK_TEXT_LINES) max_lines = OPENDECK_TEXT_LINES;
    if (!max_lines) max_lines = 1;
    const uint8_t count = opendeckTextWrap(title, lines, max_lines, rw);
    if (!count) return;
    const int block = (int)count * lh;
    int y = (int)y0 + (int)((rh > (uint32_t)block) ? (rh - block) / 2 : 0);
    for (uint8_t l = 0; l < count; l++) {
      const int tw = opendeckSprite.textWidth(lines[l]);
      opendeckSprite.drawString(lines[l], (int)x0 + (int)((rw > (uint32_t)tw) ? (rw - tw) / 2 : 0), y);
      y += lh;
    }
  }

  // An encoder's tile: what it does, and what it is at.
  //
  // The name is drawn in the biggest font that fits it on one line, so
  // "Sensitivity" is readable and "Flashes per Bar" is still readable in
  // a smaller one. A name too long even for that is cut short with an
  // ellipsis.
  void opendeckSpriteLabel(uint16_t x0, uint16_t y0, uint16_t rw, uint16_t rh,
                            const char* label, uint16_t value, bool has_value,
                            bool is_check = false) {
    if (!label || !label[0]) return;
    static const GFXfont* const fits[3] = {
      &fonts::DejaVu24, &fonts::DejaVu18, &fonts::DejaVu12
    };
    char line[OPENDECK_TEXT_CHARS];
    line[0] = 0;
    for (uint8_t f = 0; f < 3; f++) {
      opendeckSprite.setFont(fits[f]);
      if (opendeckSprite.textWidth(label) > (int)rw) continue;
      strncpy(line, label, sizeof(line) - 1);
      line[sizeof(line) - 1] = 0;
      break;
    }
    if (!line[0]) {
      // Too long even for the smallest font: cut it short.
      opendeckSprite.setFont(&fonts::DejaVu12);
      char lines[1][OPENDECK_TEXT_CHARS];
      if (!opendeckTextWrap(label, lines, 1, rw)) return;
      strncpy(line, lines[0], sizeof(line) - 1);
      line[sizeof(line) - 1] = 0;
    }
    const int lh = opendeckSprite.fontHeight();
    const int block = has_value ? lh * 2 : lh;
    const int ly = (int)y0 + (int)((rh > (uint32_t)block) ? (rh - block) / 2 : 0);
    const int tw = opendeckSprite.textWidth(line);
    opendeckSprite.drawString(line, (int)x0 + (int)((rw > (uint32_t)tw) ? (rw - tw) / 2 : 0), ly);
    if (!has_value) return;
    char val[8];
    // A check box says ON and OFF, not a number.
    if (is_check) snprintf(val, sizeof(val), "%s", value ? "ON" : "OFF");
    else snprintf(val, sizeof(val), "%u", value);
    const int vw = opendeckSprite.textWidth(val);
    opendeckSprite.drawString(val, (int)x0 + (int)((rw > (uint32_t)vw) ? (rw - vw) / 2 : 0), ly + lh);
  }

  // The wide touch strip is ONE screen: 800 by 128 pixels, cut into a
  // 200 by 128 tile per encoder. The reference composites the four
  // encoder pictures into it and sends it as one "BGPIC".
  //
  // The canvas is built here, in PSRAM, and the JPEG comes out of it.
  //
  // The JPEG and its length go into the members, the same place the key
  // pictures put them. They used to go into the caller's variables, and
  // when the picture was built on core 1 the caller's variables were a
  // local pair: the length was thrown away and the deck was told the
  // picture was NO BYTES long. The device's display controller does not
  // like that, and nothing showed on the deck at all.
  bool opendeckEncodeStrip(uint8_t** jpeg_out, uint16_t* len_out) {
    const uint16_t w = Opendeck::STRIP_W, h = Opendeck::STRIP_H;
    const size_t pic_len = (size_t)w * h * 3;
    uint8_t* buf = opendeckAlloc(pic_len);
    if (!buf) return false;
    uint8_t* jpeg = opendeckAlloc(pic_len + 16384);
    if (!jpeg) { free(buf); return false; }

    // Black, then a coloured tile per encoder with its number on it.
    if (!opendeckSpriteBegin(w, h)) { free(buf); free(jpeg); return false; }
    opendeckSprite.fillScreen(0x0000);
    // The page number takes the whole strip for a moment, the way the
    // F1's two-digit display does when it is asked to show the page.
    const uint8_t deck = deckSurface();
    if (deck < surface_count && surfaces[deck].show_page_until > millis()) {
      opendeck_page_until = surfaces[deck].show_page_until;
      char pagetext[8];
      snprintf(pagetext, sizeof(pagetext), "%u",
               surfaces[deck < surface_count ? deck : 0].page);
      opendeckSpriteTile(0, 0, w, h, pagetext, 0, 0, 0);
      opendeckSpriteToBuffer(buf, w, h);
      size_t jpeg_len = 0;
      const bool ok = opendeckEncodeJpeg(buf, w, h, jpeg, pic_len + 32768, jpeg_len);
      free(buf);
      if (!ok) { free(jpeg); return false; }
      if (jpeg_len > 0xFFFF) { free(jpeg); return false; }
      *jpeg_out = jpeg;
      *len_out = (uint16_t)jpeg_len;
      return true;
    }
    for (uint8_t i = 0; i < Opendeck::STRIP_TILES; i++) {
      const uint8_t hue = i * 36;
      const uint8_t b = (uint8_t)(127 * (1 + 0.9 * __builtin_cosf(3.14159f * (hue + 240) / 180)));
      const uint8_t g = (uint8_t)(127 * (1 + 0.9 * __builtin_cosf(3.14159f * (hue + 120) / 180)));
      const uint8_t r = (uint8_t)(127 * (1 + 0.9 * __builtin_cosf(3.14159f * hue / 180)));
      const uint16_t x0 = (uint16_t)(i * Opendeck::STRIP_TILE_W);
      // The beat that is sounding lights its tile, the way the F1's lower
      // buttons do.
      const bool beat = (currentBeat >= 1 && currentBeat <= Opendeck::STRIP_TILES &&
                         currentBeat == (int)i + 1);
      // What this tile says: the action of the encoder below it, and the
      // value that action is at. The strip is the deck's readout, so it
      // says what the encoders are doing.
      char label[20] = "";
      uint16_t value = 0;
      bool has_value = false;
      const uint8_t deck_profile = deckProfile();
      if (deck_profile < profile_count && profiles &&
          i < profiles[deck_profile].od_encoders &&
          profiles[deck_profile].encoders[i].action[0]) {
        const char* act = profiles[deck_profile].encoders[i].action;
        opendeckActionLabel(act, label, sizeof(label));
        if (!strcmp(act, "page")) {
          value = surfaces[deckSurface() < surface_count ? deckSurface() : 0].page;
          has_value = true;
        } else {
          const int cur = currentValueOf(act);
          if (cur >= 0) { value = (uint16_t)cur; has_value = true; }
        }
      }
      if (label[0]) {
        opendeckSprite.fillRect(x0, 0, Opendeck::STRIP_TILE_W, h,
            lgfx::color565(beat ? (uint8_t)(r + 0x60) : r,
                           beat ? (uint8_t)(g + 0x60) : g,
                           beat ? (uint8_t)(b + 0x60) : b));
        opendeckSprite.setTextColor(0xFFFF);
        opendeckSpriteLabel(x0, 0, Opendeck::STRIP_TILE_W, h, label, value, has_value);
      } else {
        char text[8];
        snprintf(text, sizeof(text), "%u", (unsigned)(i + 1));
        opendeckSpriteTile(x0, 0, Opendeck::STRIP_TILE_W, h, text,
                           beat ? (uint8_t)(b + 0x60) : b,
                           beat ? (uint8_t)(g + 0x60) : g,
                           beat ? (uint8_t)(r + 0x60) : r);
      }
    }
    opendeckSpriteToBuffer(buf, w, h);

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
    if (jpeg_len > 0xFFFF) { free(jpeg); return false; }
    *jpeg_out = jpeg;
    *len_out = (uint16_t)jpeg_len;
    return true;
  }

  // Draw one picture and encode it. The picture is in the encoder's
  // order (blue first) and turned 180 degrees, because that is how the
  // device reads it.
  bool opendeckEncodeKey(uint8_t index, uint8_t** jpeg_out, uint16_t* len_out) {
    const OpendeckPic& pic = opendeck_pics[index];
    const uint16_t w = pic.w, h = pic.h;
    const size_t pic_len = (size_t)w * h * 3;

    uint8_t* buf = opendeckAlloc(pic_len);
    if (!buf) return false;
    uint8_t* jpeg = opendeckAlloc(pic_len + 32768);
    if (!jpeg) { free(buf); return false; }

    // The picture is drawn with the display's own library and font.
    //
    // A key shows the TITLE of the preset it holds. The slot's number
    // goes in the corner, small, so the deck still reads as a bank of
    // presets - a title on its own says what the key does but not which
    // slot it is.
    if (!opendeckSpriteBegin(w, h)) { free(buf); free(jpeg); return false; }
    opendeckSprite.fillRect(0, 0, w, h,
                            lgfx::color565(pic.colour[2], pic.colour[1], pic.colour[0]));
    opendeckSprite.setTextColor(0xFFFF);

    // The number, in the corner, in the smallest font there is.
    char num[8];
    snprintf(num, sizeof(num), "%u", pic.number);
    opendeckSprite.setFont(&fonts::DejaVu12);
    const int num_h = opendeckSprite.fontHeight();
    opendeckSprite.drawString(num, 2, 0);

    // The title, in the rest of the key.
    opendeckSprite.setFont(&fonts::DejaVu24);
    if (pic.title[0]) {
      opendeckSpriteTitle(0, num_h, w, (uint16_t)(h - num_h), pic.title);
    } else if (pic.label[0]) {
      // An encoder's tile: what it does, and what it is at. The tile
      // belongs to one of the encoders below the strip, so it says what
      // that encoder does.
      opendeckSpriteLabel(0, num_h, w, (uint16_t)(h - num_h),
                          pic.label, pic.value, pic.has_value, pic.is_check);
    } else {
      // Nothing to say: the number, as it always was.
      char text[8];
      snprintf(text, sizeof(text), "%u", pic.number);
      opendeckSpriteTile(0, 0, w, h, text, pic.colour[0], pic.colour[1], pic.colour[2]);
    }
    if (pic.is_active) opendeckSpriteOutline(w, h, 3);
    opendeckSpriteToBuffer(buf, w, h);

    // The device reads its pictures upside down, so the picture is turned
    // 180 degrees: the rows and the columns both go.
    //
    // The turn has to be done a pair of rows at a time. Done row by row
    // in place, the second half of the picture is read from rows that have
    // already been written, and the picture comes out with its top half
    // rotated onto its bottom half.
    {
      const size_t row = (size_t)w * 3;
      uint8_t* tmp = opendeckAlloc(row);
      if (tmp) {
        for (uint16_t y = 0; y < h / 2; y++) {
          uint8_t* top = buf + (size_t)y * row;
          uint8_t* bot = buf + (size_t)(h - 1 - y) * row;
          memcpy(tmp, top, row);
          for (uint16_t x = 0; x < w; x++) {
            memcpy(top + (size_t)x * 3, bot + (size_t)(w - 1 - x) * 3, 3);
          }
          for (uint16_t x = 0; x < w; x++) {
            memcpy(bot + (size_t)x * 3, tmp + (size_t)(w - 1 - x) * 3, 3);
          }
        }
        free(tmp);
      }
    }

    size_t jpeg_len = 0;
    const bool ok = opendeckEncodeJpeg(buf, w, h, jpeg, pic_len + 32768, jpeg_len);
    free(buf);
    if (!ok) { free(jpeg); return false; }

    // A copy of the last picture, so it can be looked at: the deck's
    // screens are the only place it shows up otherwise.
    {
      File f = WLED_FS.open("/hidctl-preview.jpg", "w");
      if (f) {
        f.write(jpeg, jpeg_len);
        f.close();
      }
    }

    // The JPEG has to outlive this call: the chunks go out over the next
    // passes of the loop.
    *jpeg_out = jpeg;
    *len_out = (uint16_t)jpeg_len;
    return true;
  }

  // The pictures are built on core 1.
  //
  // Drawing a picture and encoding it as a JPEG takes long enough to trip
  // the watchdog when it happens on the main loop, and core 1 is free for
  // it. The USB transfers stay on the main loop - only the building moves.
  TaskHandle_t opendeck_build_task = nullptr;
  volatile uint8_t opendeck_build_index = 0;
  volatile bool opendeck_build_request = false;
  volatile bool opendeck_build_busy = false;   // the task is building it
  volatile bool opendeck_build_ready = false;
  volatile bool opendeck_build_failed = false;
  // Which picture the task has just finished. The main loop only takes it
  // if it is the picture it asked for.
  volatile uint8_t opendeck_build_done_index = 0xFF;

  static void opendeckBuildTask(void* arg) {
    HidctlUsermod* self = (HidctlUsermod*)arg;
    for (;;) {
      while (!self->opendeck_build_request) vTaskDelay(2);
      // The busy flag goes up BEFORE the request goes down. The main loop
      // asks for a picture when neither flag is set, so if the request
      // were cleared first there would be a moment in which the main loop
      // asked for the NEXT picture while this one was still being made -
      // and the picture asked for first never went out at all.
      self->opendeck_build_busy = true;
      self->opendeck_build_request = false;
      const uint8_t index = self->opendeck_build_index;
      const uint8_t slot = self->opendeck_pics[index].slot;
      uint8_t* jpeg = nullptr;
      uint16_t jpeg_len = 0;
      bool ok;
      if (slot == 0) {
        // The picture across the whole strip.
        ok = self->opendeckEncodeStrip(&jpeg, &jpeg_len);
      } else {
        ok = self->opendeckEncodeKey(index, &jpeg, &jpeg_len);
      }
      if (ok && jpeg) {
        // The slot's picture is replaced and the old one is let go. The
        // sender is never sending this slot at this moment: a slot is in
        // the queue once, and the queue is only built when the last push
        // is away.
        if (self->opendeck_cache[slot]) free(self->opendeck_cache[slot]);
        self->opendeck_cache[slot] = jpeg;
        self->opendeck_cache_len[slot] = jpeg_len;
        self->opendeck_cache_sig[slot] = self->opendeck_pics[index].sig;
        self->opendeck_cache_valid[slot] = true;
      }
      self->opendeck_build_failed = !ok;
      if (!ok) {
        USER_PRINTF("[HIDCTL] the picture for slot %u would not build\n",
                    (unsigned)self->opendeck_pics[index].slot);
      }
      self->opendeck_build_busy = false;
      self->opendeck_build_done_index = index;
      self->opendeck_build_ready = true;
    }
  }

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
      Opendeck::stp(opendeck_buf);
      if (opendeckSend(opendeck_buf)) {
        opendeck_painting = false;
        USER_PRINTF("[HIDCTL] deck: %u pictures are away\n", opendeck_queue_count);
      }
      return;
    }
    if (opendeck_stage == OPENDECK_STAGE_ENCODE) {
      // This screen's picture has to be made first - on core 1, so the
      // main loop is not held up while it is drawn and encoded.
      if (opendeck_build_ready) {
        opendeck_build_ready = false;
        const bool wrong = (opendeck_build_done_index != opendeck_queue_pos);
        opendeck_build_done_index = 0xFF;
        if (opendeck_build_failed || wrong) {
          // The picture would not build, or it is not the one that was
          // asked for - the queue was rebuilt while it was being made.
          // Skip it: skipping keeps the push moving.
          opendeck_build_failed = false;
          opendeck_queue_pos++;
          return;
        }
        opendeck_stage = OPENDECK_STAGE_BAT;
        return;
      }
      if (!opendeck_build_request && !opendeck_build_busy && opendeck_build_task) {
        opendeck_build_index = opendeck_queue_pos;
        opendeck_build_request = true;
        USER_PRINTF("[HIDCTL] deck: building picture %u on core 1\n",
                    (unsigned)opendeck_queue_pos);
      }
      return;                     // come back for it when it is ready
    }
    if (opendeck_stage == OPENDECK_STAGE_BAT) {
      const OpendeckPic& pic = opendeck_pics[opendeck_queue_pos];
      const uint16_t jpeg_len = opendeck_cache_len[pic.slot];
      if (!opendeck_cache[pic.slot] || !jpeg_len) {
        // There is no picture to send. Announcing one of no bytes is
        // worse than sending nothing: the device's display controller
        // does not survive it, and nothing shows on the deck at all.
        USER_PRINTF("[HIDCTL] deck: slot %u has no picture - skipped\n",
                    pic.slot);
        opendeck_queue_pos++;
        opendeck_stage = OPENDECK_STAGE_ENCODE;
        return;
      }
      if (pic.slot == 0) {
        // The wide strip: one picture across all of it.
        Opendeck::bgpic(opendeck_buf, jpeg_len, 0, 0, pic.w, pic.h);
      } else {
        Opendeck::bat(opendeck_buf, (uint8_t)(pic.slot - 1), jpeg_len);
      }
      if (!opendeckSend(opendeck_buf)) return;    // the pipe is busy: try again
      USER_PRINTF("[HIDCTL] deck: picture for slot %u (%ux%u, %u bytes)\n",
                  pic.slot, pic.w, pic.h, jpeg_len);
      opendeck_chunk_off = 0;
      opendeck_stage = OPENDECK_STAGE_CHUNK;
      return;
    }
    const uint8_t* const jpeg = opendeck_cache[opendeck_pics[opendeck_queue_pos].slot];
    const uint16_t jpeg_len = opendeck_cache_len[opendeck_pics[opendeck_queue_pos].slot];
    if (opendeck_chunk_off < jpeg_len) {
      uint16_t n = (uint16_t)(jpeg_len - opendeck_chunk_off);
      if (n > Opendeck::PACKET_SIZE) n = Opendeck::PACKET_SIZE;
      memset(opendeck_buf, 0, Opendeck::WIRE_LEN);
      memcpy(opendeck_buf + 1, jpeg + opendeck_chunk_off, n);
      if (!opendeckSend(opendeck_buf)) return;    // the pipe is busy: try again
      opendeck_chunk_off += n;
      return;
    }
    // This key is done: on to the next one.
    opendeck_queue_pos++;
    opendeck_stage = OPENDECK_STAGE_ENCODE;
  }

  void opendeckPaintKeys(bool withKeys = true) {
    if (opendeck_painting) return;         // one push at a time
    opendeckBuildQueue(withKeys);
    // Nothing changed: the deck already has every picture, and there is
    // nothing to send.
    if (!opendeck_queue_count) return;
    opendeck_painting = true;
  }

  // The playlist chaser for a deck: the key that holds the running
  // playlist pulses slowly and the key that holds the track that is
  // playing blinks fast - the same pattern the MIDI usermod uses and the
  // F1's pads follow.
  void opendeckChaser() {
    const uint8_t profile = deckProfile();
    if (profile >= profile_count || !profiles) return;
    HidctlProfile& p = profiles[profile];
    if (!p.opendeck) return;
    const bool playlist_active = (currentPlaylist >= 0);
    const int parent = playlist_active ? (int)currentPlaylist : 0;
    const int child = (int)tracked_active_preset;
    for (uint8_t i = 0; i < p.button_count; i++) {
      const HidctlButton& b = p.buttons[i];
      if (b.index >= 10 || !b.name[0]) continue;    // only the keys with screens
      const int slot = (int)pageOffset(deckSurface()) + (int)b.index + 1;
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
  void opendeckPaintKeys(bool = true) {}
  void opendeckChaser() {}
#endif

  // Decode a raw report for the active profile and act on it.
  void handleIncomingReport(uint8_t device, const uint8_t* data, uint16_t len) {
    if (!enabled || !profiles) return;
    // Which surface is this report from? The board has a hub, so more than
    // one device can be on it, and each of them has its own profile.
    const uint8_t s = surfaceForDevice(device);
    if (s == 0xFF) return;
    cur_surface = s;
    focus_surface = s;
    if (surfaces[s].profile >= profile_count) return;
    HidSurface& sf = surfaces[s];
    HidctlProfile& p = profiles[sf.profile];

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
    uint32_t was_buttons = sf.buttons;
    sf.buttons = now_buttons;

    // Modifiers are buttons whose action is only meaningful while held.
    sf.modifier_mask = 0;
    for (uint8_t i = 0; i < p.button_count; i++) {
      if (p.buttons[i].modifier) sf.modifier_mask |= (1UL << p.buttons[i].index);
    }
    bool mod_held = (now_buttons & sf.modifier_mask) != 0;

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
      if (sf.control_has_applied[i] && raw == sf.control_value[i]) continue;  // no change at all
      sf.control_value[i] = raw;

      // A step or two of jitter is not a movement. While the change stays
      // inside the deadband the baseline stays pinned where it was, so a
      // slow drift still adds up to a movement - it is measured against
      // the last value that passed the deadband, not against the last
      // value seen.
      if (sf.control_has_applied[i]) {
        int d = (int)raw - (int)sf.control_applied[i];
        if (d < 0) d = -d;
        if (d < CONTROL_DEADBAND) {
          sf.control_pending[i] = false;
          continue;
        }
      }

      // Too soon after the last one. The next report carries a newer
      // value, and that is the one worth driving.
      if (sf.control_has_applied[i] &&
          (now - sf.control_applied_ms[i]) < CONTROL_APPLY_INTERVAL_MS) {
        sf.control_pending[i] = true;
        continue;
      }

      applyControl(s, p, i, raw, now);
      sf.control_pending[i] = false;
    }

    // --- the encoder -------------------------------------------------------
    // Its byte counts the wheel's position, so the difference says which
    // way it turned. One step is one page.
    if (p.wheel_present && len > p.wheel_offset) {
      uint8_t pos = data[p.wheel_offset];
      if (sf.wheelSeen) {
        int8_t delta = (int8_t)(pos - sf.wheelLast);   // the counter wraps
        if (delta != 0) {
          USER_PRINTF("[HIDCTL] wheel %u -> %u (delta %d)\n",
                      sf.wheelLast, pos, (int)delta);
          showWheelUntil = millis() + 1000;   // show it on the display
          turnPage(s, delta);
        }
      } else {
        USER_PRINTF("[HIDCTL] wheel first value %u\n", pos);
      }
      sf.wheelLast = pos;
      sf.wheelSeen = true;
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
    if (!strcmp(action, "effectCustom3")) return strip.getMainSegment().custom3;
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

    // The pictures are built on core 1, so the main loop is not held up
    // while one is drawn and encoded.
    if (!opendeck_build_task) {
      // Priority 4, not 1: the USB task is now on this core at priority
      // 5, and the builder must not be the bottom of the pile - the
      // deck's picture cadence is what the builder is for.
      BaseType_t ok = xTaskCreatePinnedToCore(opendeckBuildTask, "hidctl_pic",
                                              8192, this, 4, &opendeck_build_task, 1);
      if (ok != pdPASS) {
        opendeck_build_task = nullptr;
        USER_PRINTLN("[HIDCTL] the picture task would not start");
      } else {
        USER_PRINTLN("[HIDCTL] the picture task is running on core 1");
      }
    }
  }

  void loop() override {
    // Which device the commands are about: the surface the console is
    // talking about.
    const uint8_t dev = focusDevice();
    // A heartbeat, so it is visible whether this loop is running at all.
    {
      static uint32_t lastBeat = 0;
      if (millis() - lastBeat > 10000) {
        lastBeat = millis();
        USER_PRINTF("[HIDCTL] loop: surfaces=%u count=%u deck=%u pending=%u\n",
                    surface_count, profile_count,
                    (deckProfile() != 0xFF) ? 1 : 0,
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
    // An OpenDeck-family deck: its session and its pictures are handled
    // here, because this is the task that owns the USB host stack.
    if (deckProfile() != 0xFF) {
      // The chaser sets the blinks, and a blink on a deck is in the
      // PICTURE, so the pictures have to go out again at the blink's rate.
      opendeckChaser();
      // The beat and the music playlist's slot: a deck shows them in its
      // pictures, and paint() does nothing for a deck.
      refreshTrackerState();
      // The strip was showing the page number: put the encoders back.
      if (opendeck_page_until && (int32_t)(millis() - opendeck_page_until) >= 0) {
        opendeck_page_until = 0;
        opendeck_pending_paint = true;
      }
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
        opendeck_pending_strip = false;
        opendeckInit();
        opendeckPaintKeys();
      } else if (opendeck_pending_paint) {
        opendeck_pending_paint = false;
        opendeck_pending_strip = false;
        opendeckPaintKeys();
      } else if (opendeck_pending_strip) {
        opendeck_pending_strip = false;
        opendeckPaintKeys(false);
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
      hid_usb_send_wakeup(dev);
    }
    if (cmd_other_iface >= 0) {
      int8_t claim = cmd_other_iface;
      cmd_other_iface = -1;
      hid_usb_set_other_interface(dev, claim != 0);
    }
  }

  // The preset changed: the pads and the display have to follow.
  void onStateChange(uint8_t mode) override {
    paint();
    // A deck's screens are pictures, so they have to be pushed again.
    // An encoder that turned a parameter changed the strip and nothing
    // else, and the ten keys are a second of pictures on their own.
    repaintDeck(opendeck_strip_change);
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
        // The pads address the page the active preset is on. Every device
        // finds the preset on ITS page: the page sizes differ, so the page
        // numbers do too.
        for (uint8_t s = 0; s < surface_count; s++) {
          if (!surfaces[s].in_use) continue;
          surfaces[s].page = pageOfPreset(slot, surfaces[s].profile);
        }
        USER_PRINTF("[HIDCTL] preset %u -> page %u (cached=%u playlist=%u music=%u)\n",
                    slot, surfaces[focus_surface < surface_count ? focus_surface : 0].page,
                    getCachedPresetExists(slot) ? 1 : 0,
                    (presetCache != nullptr && presetCache[slot].isPlaylist) ? 1 : 0,
                    music_slot);
        paint();
        repaintDeck();
      }
      return;
    }
    if (ev.type == wled::EventType::UsbDeviceChanged) {
      // The USB host's own message is the authority on which device is
      // connected: it says which device, and this event does not - it is
      // published by the MIDI host, which sees every device on the bus.
      // So the event is only news; the surfaces are bound elsewhere.
      USER_PRINTF("[HIDCTL] a device on the bus changed (connected=%u)\n",
                  ev.payload.usbDevice.connected ? 1 : 0);
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
    // Which device the commands are about: the surface the console is
    // talking about.
    const uint8_t dev = focusDevice();
    bool demo = h["demo"] | false;
    setDemo(demo);
    if (h.containsKey("paint")) paint_enabled = h["paint"] | true;
    // Send the output report over the control pipe (SET_REPORT) instead
    // of the interrupt OUT endpoint. Some devices only take it that way.
    if (h.containsKey("ctrlout")) hid_usb_set_out_via_control(dev, h["ctrlout"] | false);
    // Send the packet without its leading report-ID byte.
    if (h.containsKey("nolead")) hid_usb_set_out_drop_leading(dev, h["nolead"] | false);
    // End the packet with a zero-length packet. A device whose report is
    // exactly the size of its endpoint's maximum packet needs it.
    if (h.containsKey("zlp")) hid_usb_set_out_zlp(dev, h["zlp"] | false);
    // Move the report to the front of the packet and keep the packet's
    // length, so a pad byte ends the transfer.
    if (h.containsKey("shift")) hid_usb_set_out_shift(dev, h["shift"] | false);
    // Cap how many bytes of the packet go out. The deck's report is 1024
    // bytes and its endpoint's maximum packet is 1024, so the 1025-byte
    // packet goes out as 1024 and then a short packet of one byte. A
    // Windows HID stack drops that trailing byte, so the device may only
    // ever see 1024 - and a short packet in the middle of a stream may
    // throw its parser out.
    if (h.containsKey("outcap")) hid_usb_set_out_cap(dev, (uint16_t)(h["outcap"] | 0));
    // The gap between two packets to the deck, in ms. A PC's HID stack
    // leaves 20 ms between them and the deck may need the room.
    if (h.containsKey("outgap")) opendeck_out_gap = (uint32_t)(h["outgap"] | 0);
    // Push the pictures again. NOT a new session: the reference sends its
    // DIS once per connection and the device does not like it repeated.
    if (h["repaint"]) {
      // Every picture goes out again, not just the ones that changed.
      opendeckClearCache();
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
    if (h.containsKey("inlen")) hid_usb_set_in_xfer_len(dev, (uint16_t)(h["inlen"] | 0));
    if (h["inreset"]) hid_usb_reset_in_pipe(dev);
    // The SET_IDLE rate, in ms. Zero is what a PC asks for: the device
    // reports only when its state changes. A rate makes it report
    // whether or not anything happens, which is how a device nobody is
    // touching is told apart from one whose input path is broken.
    if (h.containsKey("idle")) hid_usb_set_idle_rate(dev, (uint16_t)(h["idle"] | 0));
    // The SET_IDLE rate in the low byte of wValue instead of the high
    // one. The specification says the high byte, but this device is not
    // a conformant HID stack.
    if (h.containsKey("idlelow")) hid_usb_set_idle_rate_low_byte(dev, h["idlelow"] | false);
    // The data toggle of the interrupt IN channel. The driver only
    // writes it for control pipes, so a pipe that has never transferred
    // keeps whatever the channel had - and a device whose toggle is out
    // of step ignores every token.
    if (h.containsKey("pid")) {
      bool ok = hid_usb_set_in_pid(dev, (uint8_t)(h["pid"] | 0));
      USER_PRINTF("[HIDCTL] the IN channel's data toggle: %s\n", ok ? "set" : "no channel");
    }
    // Ask the device for its firmware, the way the reference does.
    if (h["fwver"]) hid_usb_read_firmware(dev);
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
    if (h.containsKey("classreq")) hid_usb_set_class_requests(dev, h["classreq"] | false);
    // Claim or release the device's other interface. A PC's HID stack
    // leaves the F1's second interface alone; we claimed it on a hunch.
    // The web API runs in the async_tcp task and the USB stack belongs to
    // the main loop, so the command is carried there.
    if (h.containsKey("otheriface")) cmd_other_iface = (h["otheriface"] | true) ? 1 : 0;
    // Send the wake-up packet again - also carried to the main loop.
    if (h["a0"]) cmd_wakeup = true;
    // Ask for the feature reports (a PC's HID stack does not).
    if (h.containsKey("featrep")) hid_usb_set_feature_reports(dev, h["featrep"] | true);
    if (h.containsKey("page")) {
      int pg = h["page"] | 0;
      // The page of the device the console is talking about.
      const uint8_t s = (focus_surface < surface_count) ? focus_surface : 0;
      if (s < surface_count && pg >= 1 && pg <= (int)lastPage(surfaces[s].profile)) {
        surfaces[s].page = (uint8_t)pg;
        surfaces[s].show_page_until = millis() + 1500;
        paint();
        // A deck's screens show the page's slots, so they change with it.
        repaintDeck();
      }
    }
  }

  void addToJsonInfo(JsonObject& obj) override {
    // The info pane's rows are flat: each usermod publishes its own key in
    // "u", and only the first two elements of a row are rendered.
    JsonObject user = obj["u"];
    if (user.isNull()) user = obj.createNestedObject("u");
    JsonArray infoArr = user.createNestedArray(FPSTR(_name));
    if (focusProfile() != 0xFF) {
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
    // Which device the readings are about: the surface the console is
    // talking about.
    const uint8_t dev = focusDevice();
    top["connected"] = (focusProfile() != 0xFF);
    top["surfaces"] = surface_count;
    top["device"] = profileName();
    top["profiles_loaded"] = profile_count;
    top["buttons"] = surfaces[focus_surface < surface_count ? focus_surface : 0].buttons;
    // How many of the first 64 preset slots the core's cache knows about -
    // the pads are coloured from it.
    uint8_t cached = 0;
    for (uint8_t s = 1; s <= 64; s++) {
      if (getCachedPresetExists(s)) cached++;
    }
    top["presets_cached"] = cached;
    top["music_slot"] = music_slot;
    top["demo"] = demoActive;
    top["class_status"] = hid_usb_class_status(dev);
    top["class_stage"] = hid_usb_class_stage(dev);
    top["other_iface"] = hid_usb_other_iface(dev);
    top["other_claimed"] = hid_usb_other_claimed(dev);
    {
      uint8_t fl = 0;
      const uint8_t* fd = hid_usb_feature_data(dev, &fl);
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
      const uint8_t* cd = hid_usb_class_data(dev, &cl);
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
    top["reports_in"] = hid_usb_report_count(dev);
    top["zlps"] = hid_usb_zlp_count(dev);
    top["out_sent"] = hid_usb_out_count(dev);
    top["out_status"] = hid_usb_last_out_status(dev);
    top["out_no_device"] = hid_usb_out_refused(dev, 0);
    top["out_too_long"] = hid_usb_out_refused(dev, 1);
    top["out_busy"] = hid_usb_out_refused(dev, 2);
    top["out_submit_err"] = hid_usb_out_refused(dev, 3);
    top["out_submit_code"] = hid_usb_out_submit_error(dev);
    top["out_stuck"] = hid_usb_out_stuck(dev);
    top["ctrl_out"] = hid_usb_ctrl_out_count(dev);
    top["out_bytes"] = hid_usb_out_bytes(dev);
    top["out_via_control"] = hid_usb_get_out_via_control(dev) ? 1 : 0;
    top["out_drop_leading"] = hid_usb_get_out_drop_leading(dev) ? 1 : 0;
    top["out_zlp"] = hid_usb_get_out_zlp(dev) ? 1 : 0;
    top["in_xfer_len"] = hid_usb_get_in_xfer_len(dev);
    top["idle_rate"] = hid_usb_get_idle_rate(dev);
    top["idle_low"] = hid_usb_get_idle_rate_low_byte(dev) ? 1 : 0;
    // The DWC2's own registers: whether the deck's IN channel is polled
    // at all, and what the controller thinks of it.
    {
      static HidctlDwcProbe probe;
      hidctlDwcSnapshot(probe);
      JsonObject d = top.createNestedObject("dwc");
      if (probe.ok) {
        d["rx_fifo"] = probe.grxfsiz;      // the RX FIFO, in lines
        d["nptx_fifo"] = probe.gnptxfsiz;
        d["ptx_fifo"] = probe.hptxfsiz;
        d["gintsts"] = probe.gintsts;
        d["hcfg"] = probe.hcfg;
        d["hfnum"] = probe.hfnum;
        d["haint"] = probe.haint;
        d["hflbaddr"] = probe.hflbaddr;
        d["channels"] = probe.channel_count;
        JsonArray fl = d.createNestedArray("frame_list");
        for (uint8_t i = 0; i < 4; i++) fl.add(probe.frame_list[i]);
        JsonArray chans = d.createNestedArray("chan");
        for (uint8_t c = 0; c < 16; c++) {
          const HidctlDwcChannel& cc = probe.channels[c];
          if (!cc.hcchar) continue;
          JsonObject co = chans.createNestedObject();
          co["n"] = c;
          co["chena"] = cc.chena;
          co["ep"] = cc.epnum;
          co["dir"] = cc.epdir;             // 1 = IN
          co["type"] = cc.eptype;          // 1 = interrupt
          co["mps"] = cc.mps;
          co["addr"] = cc.devaddr;
          co["hcint"] = cc.hcint;
          co["size"] = cc.xfersize;
          co["pkts"] = cc.pktcnt;
          co["pid"] = cc.pid;
          co["frames"] = cc.frames;        // how many frames poll it
          co["qtd"] = cc.qtd_status;
          co["qtdbuf"] = cc.qtd_buffer;
          co["qtdfld"] = cc.qtd_fields;
          co["left"] = cc.qtd_remaining;
        }
      }
    }
    top["out_shift"] = hid_usb_get_out_shift(dev) ? 1 : 0;
    top["out_cap"] = hid_usb_get_out_cap(dev);
    top["out_gap"] = opendeck_out_gap;
    top["link_speed"] = hid_usb_link_speed(dev);
    {
      uint8_t fl = 0;
      const uint8_t* fb = hid_usb_out_first(dev, &fl);
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
      const uint8_t* dd = hid_usb_report_desc(dev, &dl);
      char hex[2 * 256 + 1];
      int hl = 0;
      for (uint16_t i = 0; i < dl && hl < (int)sizeof(hex) - 3; i++) {
        hl += snprintf(hex + hl, sizeof(hex) - hl, "%02X", dd[i]);
      }
      top["report_desc"] = hex;
      uint8_t ol = 0;
      const uint8_t* oh = hid_usb_last_out_head(dev, &ol);
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
    top["page"] = surfaces[focus_surface < surface_count ? focus_surface : 0].page;
    top["page_count"] = lastPage(focusProfile());
    // The deck's encoders: what each one drives, what its tile on the
    // strip says, and what it is at. The label is the effect's own name
    // for the control, so it changes with the effect.
    const uint8_t deck_profile = deckProfile();
    if (deck_profile < profile_count && profiles && profiles[deck_profile].opendeck) {
      JsonArray encs = top.createNestedArray("encoders");
      const uint8_t count = profiles[deck_profile].od_encoders;
      for (uint8_t i = 0; i < count && i < HIDCTL_MAX_ENCODERS; i++) {
        const char* act = profiles[deck_profile].encoders[i].action;
        char label[20] = "";
        opendeckActionLabel(act, label, sizeof(label));
        JsonObject e = encs.createNestedObject();
        e["name"] = profiles[deck_profile].encoders[i].name;
        e["action"] = act;
        e["label"] = label;
        e["value"] = currentValueOf(act);
      }
    }
    // The preset that is active, tracked across the core's wiping of
    // currentPreset - this is the one the green pad shows.
    top["active_preset"] = tracked_active_preset;
    top["report_status"] = hid_usb_last_report_status(dev);
    top["reports_ok"] = hid_usb_reports_ok(dev);
    uint8_t rb_len = 0;
    const uint8_t* rb = hid_usb_readback(dev, &rb_len);
    if (rb_len) {
      char hex[2 * HIDCTL_READBACK_BYTES + 1];
      int hl = 0;
      for (uint8_t i = 0; i < rb_len && hl < (int)sizeof(hex) - 3; i++) {
        hl += snprintf(hex + hl, sizeof(hex) - hl, "%02X", rb[i]);
      }
      top["readback"] = hex;
    }
    uint8_t out_len = 0;
    const uint8_t* out = hid_usb_last_out_report(dev, &out_len);    if (out_len) {
      char hex[2 * HIDCTL_READBACK_BYTES + 1];
      int hl = 0;
      for (uint8_t i = 0; i < out_len && hl < (int)sizeof(hex) - 3; i++) {
        hl += snprintf(hex + hl, sizeof(hex) - hl, "%02X", out[i]);
      }
      top["led_report"] = hex;
    }
    // Every surface on the bus: which device it is on, which profile it
    // uses, the page it is on, and what has gone in and out of it.
    JsonArray devs = top.createNestedArray("devices");
    for (uint8_t i = 0; i < surface_count; i++) {
      if (!surfaces[i].in_use) continue;
      JsonObject d = devs.createNestedObject();
      d["device"] = surfaces[i].device;
      d["profile"] = (surfaces[i].profile < profile_count)
                       ? profiles[surfaces[i].profile].name : "";
      d["deck"] = surfaces[i].opendeck;
      d["page"] = surfaces[i].page;
      d["page_count"] = lastPage(surfaces[i].profile);
      d["buttons"] = surfaces[i].buttons;
      d["in_reports"] = hid_usb_report_count(surfaces[i].device);
      d["out_sent"] = hid_usb_out_count(surfaces[i].device);
      d["out_bytes"] = hid_usb_out_bytes(surfaces[i].device);
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
    top["in_errors"] = hid_usb_in_errors(dev);
    {
      uint8_t sc = 0;
      uint16_t* lens = nullptr;
      const uint8_t* sts = hid_usb_in_statuses(dev, &sc, &lens);
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
    const uint8_t* cfg = hid_usb_config_desc(dev, &cfg_len);
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
    const HidctlIfaceInfo* ifs = hid_usb_interfaces(dev, &ifcount);
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
    const uint8_t profile = focusProfile();
    if (profile < profile_count) {
      HidctlProfile& p = profiles[profile];
      JsonObject vals = top.createNestedObject("values");
      const uint8_t s = (focus_surface < surface_count) ? focus_surface : 0;
      for (uint8_t i = 0; i < p.control_count; i++) {
        vals[p.controls[i].name] = surfaces[s].control_value[i];
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

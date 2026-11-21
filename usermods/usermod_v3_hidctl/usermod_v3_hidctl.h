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

  // Move a page forward (1) or back (-1) and take the current preset
  // with it, keeping its place in the page.
  void turnPage(int direction) {
    const uint8_t last = lastPage();
    int want = (int)page + direction;
    if (want < 1) want = 1;
    if (want > last) want = last;
    if (want == (int)page) return;
    page = (uint8_t)want;

    // The preset keeps its place in the page, so it moves by sixteen.
    uint16_t offset = 0;
    if (tracked_active_preset >= 1) {
      offset = (uint16_t)((tracked_active_preset - 1) % PAGE_SIZE);
    }
    uint16_t target = (uint16_t)((page - 1) * PAGE_SIZE + offset + 1);
    if (target > 250) target = 250;

    // Land on a preset that exists: the one in the same place on this
    // page, or else the first one that is there. A page with nothing
    // saved on it is browsed without touching the running preset.
    uint16_t pick = 0;
    if (getCachedPresetExists((uint8_t)target)) {
      pick = target;
    } else {
      for (uint16_t s = (uint16_t)((page - 1) * PAGE_SIZE + 1);
           s <= (uint16_t)(page * PAGE_SIZE) && s <= 250; s++) {
        if (getCachedPresetExists((uint8_t)s)) { pick = s; break; }
      }
    }

    if (pick) {
      // The core's PresetApplied event sets the tracked preset and the
      // page; applying it here is what moves both.
      applyPreset((uint8_t)pick, CALL_MODE_BUTTON_PRESET);
      handlePresets();
    }
    showPageUntil = millis() + 1500;
    USER_PRINTF("[HIDCTL] page %u (preset %u)\n", page, pick);
    // The page is on the display for a moment, so paint it now - and the
    // pads move to the new page's presets whether or not there is a
    // preset there to apply.
    paint();
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
    p.report_len = (uint8_t)(j["report_len"] | 0);
    if (p.report_len == 0 || p.report_len > HIDCTL_MAX_REPORT) return false;

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
      if (ctl.bits == 0 || ctl.bits > 16) return false;
      if (ctl.offset + ((ctl.bits + 7) / 8) > p.report_len) return false;
      p.control_count++;
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
    if (!WLED_FS.exists(filename)) {
      File f = WLED_FS.open(filename, "w");
      if (!f) {
        USER_PRINTF("[HIDCTL] could not write default profile %s\n", filename);
        return;
      }
      f.print(content);
      f.close();
      USER_PRINTF("[HIDCTL] wrote default profile %s\n", filename);
    }
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
        USER_PRINTF("[HIDCTL] profile %u: %s (0x%04X:0x%04X) %u controls, %u buttons\n",
                    profile_count, profiles[profile_count].name,
                    profiles[profile_count].vid, profiles[profile_count].pid,
                    profiles[profile_count].control_count,
                    profiles[profile_count].button_count);
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
      if (slot < 1 || slot > 64) return;
      char nm[40];
      snprintf(nm, sizeof(nm), "HIDCTL preset %d", slot);
      savePreset((uint8_t)slot, nm);
      stateUpdated(CALL_MODE_BUTTON_PRESET);
      return;
    }
    if (!strncmp(action, "preset", 6)) {
      int slot = atoi(action + 6);
      if (slot < 1 || slot > 64) return;
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
      bri = 0;
      strip.setBrightness(0, true);
      stateUpdated(CALL_MODE_BUTTON);
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
    // The strokes of a "P": top, upper-left, middle, lower-left - and NO
    // bottom bar, which is what tells it from an "E".
    uint8_t mask = 0;
    switch (letter) {
      case 'P': mask = (1u << 4) | (1u << 5) | (1u << 1) | (1u << 6); break;
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
    // one of them.
    static uint32_t lastPaint = 0;
    if (millis() - lastPaint < 100) return;
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
    } else {
      showNumber(report, p, val);
    }

    if (demoActive) {
      paintDemo(report, p);
      hid_usb_send_out_endpoint(report, p.output.length);
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

    hid_usb_send_out_endpoint(report, p.output.length);
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

    // Soft takeover: the control has to cross the parameter's current
    // value before it starts driving it, so a knob that sits somewhere
    // else does not yank the value when it is first touched.
    if (!control_taken[idx]) {
      int cur = currentValueOf(ctl.action);
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

    if (!ctl.action[0]) return;
    if (now - control_log_ms[idx] >= CONTROL_LOG_INTERVAL_MS) {
      control_log_ms[idx] = now;
      USER_PRINTF("[HIDCTL] %s = %u -> %s = %u\n", ctl.name, raw, ctl.action, value);
    }
    runAction(ctl.action, value);
  }

  // Decode a raw report for the active profile and act on it.
  void handleIncomingReport(const uint8_t* data, uint16_t len) {
    if (!enabled || active_profile >= profile_count || !profiles) return;
    HidctlProfile& p = profiles[active_profile];

    if (len < p.report_len) return;
    if (p.report_id && data[0] != p.report_id) {
      // A report we did not ask for. Say so: the device may send the
      // encoder's position in a report of its own.
      USER_PRINTF("[HIDCTL] report id 0x%02X (%u bytes): %s\n", data[0], len,
                  bytes_to_hex(data, len < 8 ? len : 8));
      return;
    }

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

    // --- buttons ------------------------------------------------------------
    uint32_t now_buttons = 0;
    for (uint8_t bi = 0; bi < p.button_byte_count; bi++) {
      uint8_t byte = data[p.button_bytes[bi]];
      for (uint8_t bit = 0; bit < 8; bit++) {
        bool set = p.msb_first ? (byte & (0x80 >> bit)) : (byte & (1 << bit));
        if (set) now_buttons |= (1UL << (bi * 8 + bit));
      }
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

    uint32_t pressed = now_buttons & ~buttons;
    buttons = now_buttons;

    // Modifiers are buttons whose action is only meaningful while held.
    modifier_mask = 0;
    for (uint8_t i = 0; i < p.button_count; i++) {
      if (p.buttons[i].modifier) modifier_mask |= (1UL << p.buttons[i].index);
    }
    bool mod_held = (now_buttons & modifier_mask) != 0;

    if (pressed) {
      for (uint8_t i = 0; i < p.button_count; i++) {
        const HidctlButton& btn = p.buttons[i];
        if (btn.modifier) continue;              // modifiers act on release
        if (!(pressed & (1UL << btn.index))) continue;
        const char* act = (mod_held && btn.mod_action[0]) ? btn.mod_action : btn.action;

        char shifted[HIDCTL_MAX_ACTION + 8];
        const char* use = act;
        if (!strncmp(act, "preset", 6) && isdigit(act[6])) {
          uint16_t slot = (uint16_t)(atoi(act + 6) + pageOffset());
          snprintf(shifted, sizeof(shifted), "preset%u", slot);
          use = shifted;
        }
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
    loadProfiles();
  }

  void loop() override {
    // A control value held back by the throttle is applied here too, in
    // case the controller has stopped reporting.
    flushPendingControls();
    // The beat counter on the lower buttons moves on its own, so the LEDs
    // have to be repainted even when nothing else changes. paint() rate
    // limits itself.
    paint();
  }

  // The preset changed: the pads and the display have to follow.
  void onStateChange(uint8_t mode) override {
    paint();
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
        paint();
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
    top["wheel"] = wheelSeen ? wheelLast : 0;
    top["wheel_seen"] = wheelSeen;
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
    const uint8_t* out = hid_usb_last_out_report(&out_len);
    if (out_len) {
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

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

  // Websocket push throttle while a control is moving.
  uint32_t lastWsPush = 0;
  static constexpr uint32_t WS_PUSH_INTERVAL_MS = 250;

  // Console log throttle for the continuous controls.
  uint32_t lastCtlLog = 0;

  // The beat that is sounding (1..4), from the tempo tracker. The lower
  // buttons show it.
  uint8_t currentBeat = 0;

  // The AutoPlaylist's music slot, for the pad colours.
  uint8_t music_slot = 0;

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
      p.button_bytes[p.button_byte_count++] = (uint8_t)(v.as<int>());
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
      const char* bn = b["name"];
      copyStr(btn.name, bn);
      const char* act = b["action"];
      copyAction(btn.action, act);
      const char* mod = b["mod_action"];
      copyAction(btn.mod_action, mod);
      btn.modifier = b["modifier"] | false;
      p.button_count++;
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
    for (uint8_t i = 0; i < HIDCTL_MAX_CONTROLS; i++) {
      control_value[i] = 0xFFFF;   // force the first report to apply
      control_taken[i] = 0;
    }
    buttons = 0;
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
      currentPreset = (uint8_t)slot;
      applyPreset(currentPreset, CALL_MODE_BUTTON_PRESET);
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
    if (currentPreset < 1) currentPreset = 1;
    int start = currentPreset;
    for (int i = 0; i < 250; i++) {
      currentPreset += direction;
      if (currentPreset < 1)   currentPreset = 250;
      if (currentPreset > 250) currentPreset = 1;
      if (currentPreset == start) break;
      if (getCachedPresetExists(currentPreset)
          && !(presetCache != nullptr && presetCache[currentPreset].isPlaylist)) {
        break;
      }
    }
    if (getCachedPresetExists(currentPreset)
        && !(presetCache != nullptr && presetCache[currentPreset].isPlaylist)) {
      applyPreset(currentPreset, CALL_MODE_BUTTON_PRESET);
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
  void probeFormats() {
    if (!profiles || active_profile >= profile_count) return;
    HidctlProfile& p = profiles[active_profile];
    if (!p.output.present) return;

    uint8_t buf[HIDCTL_MAX_OUTPUT];
    memset(buf, 0, sizeof(buf));
    uint8_t* data = buf;   // the report's data; the ID goes in wValue

    // Everything on: the display, the buttons, the pads.
    for (uint8_t i = 0; i < 16 && i < p.output.length; i++) data[i] = 0x7F;
    for (uint8_t i = 16; i < 24 && i < p.output.length; i++) data[i] = 0x7F;
    for (uint8_t i = 72; i < 80 && i < p.output.length; i++) data[i] = 0x7F;
    for (uint8_t pad = 0; pad < 16; pad++) {
      uint16_t base = 24 + pad * 3;
      if (base + 2 < p.output.length) {
        data[base + 0] = 0x00;   // blue
        data[base + 1] = 0x7F;   // red
        data[base + 2] = 0x00;   // green
      }
    }

    // The LED report, in the layout NI's own reverse engineering found: 16
    // bytes of display strokes, 8 bytes of special buttons, the 16 pads as
    // three bytes each (blue, red, green) and the four lower buttons, two
    // sides each. Light the pads red and the buttons white.
    memset(buf, 0, sizeof(buf));
    for (uint8_t i = 16; i < 24; i++) buf[i] = 0xFF;      // the 8 buttons
    for (uint8_t pad = 0; pad < 16; pad++) {
      uint16_t base = 24 + pad * 3;
      if (base + 2 < p.output.length) {
        buf[base + 0] = 0x00;   // blue
        buf[base + 1] = 0xFF;   // red
        buf[base + 2] = 0x00;   // green
      }
    }
    for (uint8_t i = 72; i < 80 && i < p.output.length; i++) buf[i] = 0xFF;

    // The F1's LED report. It goes out on the interface's interrupt OUT
    // endpoint as the whole report: the ID byte first, then the data.
    //
    //   byte 0        report ID 0x80
    //   bytes 1..7    the left display, seven segments
    //   byte 8        the dot between the displays
    //   bytes 9..15   the right display, seven segments
    //   byte 16       (not identified)
    //   bytes 17..24  Browse, Size, Type, Reverse, Shift, Capture, Quant, Sync
    //   bytes 25..72  the 16 pads, three bytes each
    //   bytes 73..80  the four lower buttons, two bytes each (left, right),
    //                 in the order 4, 3, 2, 1
    //
    // Every byte is a brightness from 00 to 7F.
    //
    // This run identifies the details that are still unknown: the order of
    // the seven strokes inside a digit. One segment at a time, byte 1
    // through byte 7, then the dot byte 8.
    uint8_t report[HIDCTL_MAX_OUTPUT];
    for (uint8_t step = 0; step < 8; step++) {
      memset(report, 0, sizeof(report));
      report[0] = 0x80;
      report[1 + step] = 0x7F;
      hid_usb_send_out_endpoint(report, 81);
      delay(1800);
      USER_PRINTF("[HIDCTL] probe: segment byte %d alone\n", 1 + step);
    }

    bool ok = hid_usb_send_out_endpoint(report, 81);
    delay(2000);
    USER_PRINTF("[HIDCTL] probe: OUT endpoint, 81 bytes (identification) -> %s, status=%d\n",
                ok ? "sent" : "FAILED", (int)hid_usb_last_out_status());
    if (probe_result_count < HIDCTL_MAX_PROBE_RESULTS) {
      probe_results[probe_result_count].type = 1;
      probe_results[probe_result_count].id = 0;
      probe_results[probe_result_count].len = 81;
      probe_results[probe_result_count].status = hid_usb_last_out_status();
      probe_result_count++;
    }
  }

  // Paint the LEDs and the display, then send the output report.
  //
  // The report goes out on the interface's interrupt OUT endpoint as the
  // whole report: the ID byte first, then the data. The device does not
  // answer HID class requests (SET_REPORT stalls on every report ID and
  // length), so the OUT endpoint is the only way in.
  void paint() {
    if (!profiles || active_profile >= profile_count) return;
    HidctlProfile& p = profiles[active_profile];
    if (!p.output.present) return;

    // WLED fires a state change often; the LEDs do not need to follow every
    // one of them.
    static uint32_t lastPaint = 0;
    if (millis() - lastPaint < 100) return;
    lastPaint = millis();

    uint8_t report[HIDCTL_MAX_OUTPUT];
    memset(report, 0, sizeof(report));
    report[0] = p.output.report_id;

    // The beat that is sounding now, from the tempo tracker.
    {
      ArTempoFrame tf;
      currentBeat = 0;
      if (arCopyTempo(&tf) && tf.beatInBar >= 1 && tf.beatInBar <= 4) {
        currentBeat = tf.beatInBar;
      }
    }
    music_slot = queryAutoPlaylistMusicSlot();

    // --- the display: the current preset number ---
    if (p.output.display.present) {
      uint16_t val = (currentPreset >= 1 && currentPreset <= 99) ? currentPreset : 0;
      uint16_t divisor = 1;
      for (uint8_t k = 1; k < p.output.display.digit_count; k++) divisor *= 10;
      for (uint8_t d = 0; d < p.output.display.digit_count; d++) {
        uint8_t digit = (uint8_t)((val / divisor) % 10);
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
        int slot = atoi(nm + 3);
        if (slot >= 1 && slot <= 64) {
          const bool is_music = (music_slot > 0 && slot == (int)music_slot);
          const bool is_playlist = presetIsPlaylist((uint8_t)slot);
          if (is_music) {
            r = g = 0x7F;                    // yellow
          } else if (is_playlist) {
            const bool finite = getPresetRepeat((uint8_t)slot) > 0;
            if (finite) { r = b = 0x7F; }      // red
            else        { r = b = 0x7F; }      // magenta
          } else if (slot == (int)currentPreset && getCachedPresetExists(slot)) {
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
  // Decode a raw report for the active profile and act on it.
  void handleIncomingReport(const uint8_t* data, uint16_t len) {
    if (!enabled || active_profile >= profile_count || !profiles) return;
    HidctlProfile& p = profiles[active_profile];

    if (len < p.report_len) return;
    if (p.report_id && data[0] != p.report_id) return;

    // --- continuous controls ------------------------------------------------
    for (uint8_t i = 0; i < p.control_count; i++) {
      HidctlControl& ctl = p.controls[i];
      uint16_t raw = 0;
      uint8_t nbytes = (ctl.bits + 7) / 8;
      for (uint8_t b = 0; b < nbytes; b++) {
        raw |= (uint16_t)((uint16_t)data[ctl.offset + b] << (8 * b));   // little endian
      }
      if (ctl.bits < 16) raw &= (uint16_t)((1u << ctl.bits) - 1);
      if (raw == control_value[i]) continue;
      control_value[i] = raw;

      if (!ctl.action[0]) continue;
      uint32_t full = (ctl.bits >= 16) ? 0xFFFFu : ((1u << ctl.bits) - 1);
      uint8_t value = (uint8_t)((uint32_t)raw * 255u / full);
      // A knob sweep reports ~100 times a second; log it at a readable rate.
      if (millis() - lastCtlLog > 250) {
        USER_PRINTF("[HIDCTL] %s = %u -> %s = %u\n", ctl.name, raw, ctl.action, value);
        lastCtlLog = millis();
      }

      // Soft takeover: the control has to cross the parameter's current
      // value before it starts driving it, so a knob that sits somewhere
      // else does not yank the value when it is first touched.
      if (!control_taken[i]) {
        int cur = currentValueOf(ctl.action);
        if (cur >= 0) {
          int curRaw = (int)((uint32_t)cur * full / 255u);
          int delta = (int)raw - curRaw;
          if (delta < 0) delta = -delta;
          int wiggle = (int)(full / 255u * 8u) + 1;   // ~8/255 of the range
          if (delta > wiggle) continue;
        }
        control_taken[i] = 1;
      }
      runAction(ctl.action, value);
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
        USER_PRINTF("[HIDCTL] %s -> %s\n", btn.name, act);
        runAction(act, 255);
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
    // The beat counter on the lower buttons moves on its own, so the LEDs
    // have to be repainted even when nothing else changes. paint() rate
    // limits itself.
    paint();
  }

  // The preset changed: the pads and the display have to follow.
  void onStateChange(uint8_t mode) override {
    paint();
  }

  void onEvent(const wled::Event& ev) override {
    if (ev.type == wled::EventType::UsbDeviceChanged) {
      bool was = active_profile != 0xFF;
      if (ev.payload.usbDevice.connected) {
        uint8_t idx = matchDevice(ev.payload.usbDevice.vid, ev.payload.usbDevice.pid);
        if (idx != 0xFF) {
          active_profile = idx;
          for (uint8_t i = 0; i < HIDCTL_MAX_CONTROLS; i++) {
            control_value[i] = 0xFFFF;   // force the next report to apply
            control_taken[i] = 0;
          }
          buttons = 0;
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

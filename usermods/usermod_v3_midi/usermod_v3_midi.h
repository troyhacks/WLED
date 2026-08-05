// usermod_v3_midi.h
// USB-MIDI control surface usermod for WLED-MoonModules on ESP32-P4.
//
// Designed around the Akai APC Mini MK2 (8x8 RGB pad grid + 9 faders +
// 8 track buttons + 8 scene launch buttons + shift). Default mapping:
//
//   Pads 0..63         (notes 0x00..0x3F)  -> Presets 1..64
//   Fader 1            (CC 0x30)           -> effectSpeed
//   Fader 2            (CC 0x31)           -> effectIntensity
//   Fader 3            (CC 0x32)           -> effectCustom1 (main segment)
//   Fader 4            (CC 0x33)           -> effectCustom2 (main segment)
//   Fader 5            (CC 0x34)           -> effectCustom3 (0..31)
//   Fader 6            (CC 0x35)           -> effectPalette
//   Faders 7..8        (CC 0x36..0x37)     -> unused
//   Fader 9 (Master)   (CC 0x38)           -> Global brightness (bri)
//
//   Track 1..3         (notes 100..102)    -> toggleCheck1/2/3
//   Track 4            (note 103)          -> fullRepaint (shift+ = reboot arm)
//   Track 5..6         (notes 104..105)    -> prev/next effect (shift+ = palette)
//   Track 7..8         (notes 106..107)    -> prev/next preset (skips empty + finite playlists)
//   Track 9 (Shift)    (note 122)          -> shift modifier (hold + pad = save)
//
//   Scene 1..5         (notes 112..116)    -> toggleMirrorX/ReverseX/MirrorY/ReverseY/Transpose
//   Scene 6            (note 117)          -> unused
//   Scene 7            (note 118)          -> selectMode (hold to copy preset)
//   Scene 8            (note 119)          -> power on/off
//
// Destructive actions use a two-press confirmation pattern (see the
// delete_armed_pad / reboot_armed state machines): first press arms,
// pad/button flashes red-fast-blink; releasing the modifier or waiting
// for the 5s timeout cancels; second press confirms.
//
// Build flag: -D USERMOD_MIDI_USB  (gated in usermods_list.cpp)
// Target env: env:esp32p4_8MB_troyhacks  (platformio_override.ini)

#pragma once

#include "wled.h"
#include "midi_usb_host.h"  // midi_out_queue() for pad/track feedback

// Forward-declared hook for cross-usermod state. Defined in
// usermod_v2_auto_playlist.h when AutoPlaylist is compiled in.
class AutoPlaylistUsermod;
extern AutoPlaylistUsermod* autoPlaylistUsermodPtr;

// Single source of truth for the action verbs. The same strings are
// used by:
//   * runAction() — dispatch via strcmp.
//   * The track/scene default action assignments in the constructor.
//   * appendConfigData() — emitted into the GUI dropdown.
// Add a new action here AND add a matching runAction() branch, and the
// GUI dropdown picks it up automatically.
static constexpr const char* kGUIVerbs[] = {
  "power", "blackout", "full", "nightlight",
  "nextfx", "prevfx", "nextpal", "prevpal",
  "nextpreset", "prevpreset",
  "toggleCheck1", "toggleCheck2", "toggleCheck3",
  "toggleMirrorX", "toggleReverseX",
  "toggleMirrorY", "toggleReverseY",
  "toggleTranspose", "toggleFreeze",
  "fullRepaint", "rebootArm", "selectMode",
};
static constexpr uint8_t kNumGUIVerbs = sizeof(kGUIVerbs) / sizeof(kGUIVerbs[0]);

// Playlist globals from wled00/playlist.cpp — used to detect one-shot
// playlists (playlistRepeat == 1, plays once and stops) so we can
// paint them in a distinct color from regular looping playlists.
// playlistEndPreset is also read in case we want to differentiate
// "one-shot with no end preset" from "one-shot that applies an end
// preset" later.
extern byte playlistRepeat;
extern byte playlistEndPreset;

// Per-preset playlist repeat is read directly from the WLED core's
// preset cache via getPresetRepeat(slot) (see fcn_declare.h /
// presets.cpp).

// Music playlist state. Populated by queryAutoPlaylist() (defined in
// midi_usb_host.cpp so the full AutoPlaylistUsermod definition is
// visible at the call site — auto_playlist.h transitively pulls in
// audio_reactive.h which has plain globals that break the link if
// included from multiple TUs).
struct AutoPlaylistState {
  bool   present       = false;   // AutoPlaylist is compiled in + registered
  bool   active        = false;   // AutoChange on, OR autoChangeIds non-empty
  bool   autoChange    = false;   // AutoPlaylist's autoChange flag
  uint8_t musicPlaylist = 0;       // the configured music playlist slot
  bool   musicIsActive = false;   // current WLED playlist == musicPlaylist AND usermod is active
};
AutoPlaylistState queryAutoPlaylist();

class MidiUsermod : public Usermod {
 private:
  bool initDone = false;

  // ---------------------------------------------------------------------------
  // Persistent config
  // ---------------------------------------------------------------------------
  bool     enabled                = true;
  bool     feedback_enabled      = true;

  // Action gate flags — configurable from the usermod settings page.
  // Defaults match Troy's preference:
  //   copy_enabled   = true  (Scene 7 + pad + pad = copy preset)
  //   delete_enabled = true  (shift + loaded pad = delete preset)
  //   reboot_enabled = true  (shift + assigned button = arm/execute)
  // Set any to false in settings to disable the corresponding flow.
  bool     copy_enabled              = true;
  bool     delete_enabled            = true;
  bool     reboot_enabled            = true;

  // "Music" playlist slot — the pad mapped to this preset is lit yellow
  // instead of the default magenta, so the user can find their music
  // playlist at a glance. The slot is read live from the AutoPlaylist
  // usermod (queryAutoPlaylist().musicPlaylist); a fallback for builds
  // without AutoPlaylist could be added here if needed.

  // Select-mode runtime state for the preset-copy flow.
  // Hold Scene 7 to activate (a "hold to activate" gesture like shift);
  // press pad A (source); press pad B (destination); preset A is copied
  // to slot B if B is empty. Release Scene 7 to exit.
  bool     select_mode_active        = false;
  uint8_t  select_source_pad         = 0xFF;  // 0xFF = no source selected yet

  // Copy-failure flash. When a copy attempt fails (destination exists,
  // JSON buffer busy, source missing from file, file write failed,
  // etc.), the destination pad index is remembered here so paintPads()
  // can paint it red-fast-blink as long as the select key is held.
  // Cleared on Scene 7 release.
  int8_t   copy_failed_pad           = -1;    // -1 = no failure pending

  // Reboot arm-and-execute state. First press of Shift + Scene 1 sets
  // `reboot_armed = true` and starts a slow flash on Scene 1 LED. Second
  // press (within REBOOT_ARM_TIMEOUT_MS) turns off all button LEDs and
  // calls ESP.restart(). Timeout (or any other button press) disarms.
  bool     reboot_armed              = false;
  uint32_t reboot_armed_ms           = 0;
  static constexpr uint32_t REBOOT_ARM_TIMEOUT_MS = 5000;

  // Delete confirmation state. First press of Shift + a loaded pad
  // arms the delete: the pad flashes red (fast-blink 0x9B, color 5)
  // until either (a) shift is released (cancel), (b) the timeout
  // elapses (cancel), or (c) the same pad is pressed again while
  // shift is still held (confirm and delete). -1 means no pad is
  // currently armed for delete.
  int8_t   delete_armed_pad          = -1;
  uint32_t delete_armed_ms           = 0;
  static constexpr uint32_t DELETE_ARM_TIMEOUT_MS = 5000;

  // Target segment for the toggleMirrorX / toggleReverseX / toggleMirrorY
  // / toggleReverseY / toggleTranspose actions. Defaults to the main
  // segment (index 0). Exposed via config so multi-segment setups can be
  // supported later without code changes — for now the user just sets
  // the index they want toggled.
  uint8_t  target_segment           = 0;

  // Soft takeover for faders. Default true — protects against sudden jumps
  // when a fader is touched at a position different from the current WLED
  // parameter value. Until the fader crosses the current value (within a
  // ±SOFT_TAKEOVER_WIGGLE window), incoming CC packets are ignored. Once
  // the fader has crossed, subsequent CCs drive the parameter as normal.
  // Per-CC tracking — each of the 128 CCs has its own takeover state.
  bool     soft_takeover_enabled     = true;
  uint8_t  cc_taken_over[128];       // 0 = not taken over, 1 = taken over
  static constexpr uint8_t SOFT_TAKEOVER_WIGGLE = 8;  // ±8 CC steps (±16 WLED units)

  // Pads 0..63 -> preset ID 1..64. -1 means unused.
  int8_t   pad_to_preset[64];

  // Optional CC-based "save preset" path. When save_preset_cc > 0 and an
  // incoming CC equals that number with a value in 1..16, save the current
  // WLED state to that preset slot. Useful for controllers without a shift
  // button (e.g. Donner Starrypad). 0 = disabled.
  uint8_t  save_preset_cc        = 0;

  // Track buttons 0x64..0x6B = 100..107. Each maps to an action verb string.
  // Scene launch buttons 0x70..0x77 = 112..119. Same verb vocabulary.
  char     track_button_action[8][16];
  char     track_button_shift_action[8][16];   // same shape, but action runs when shift is held
  char     scene_button_action[8][16];
  char     scene_button_shift_action[8][16];

  // CC# 0..127 -> action string. Most are unused.
  char     cc_to_action[128][16];

  // ---------------------------------------------------------------------------
  // Runtime state
  // ---------------------------------------------------------------------------
  bool        midi_connected              = false;
  bool        shift_held                  = false;
  uint16_t    midi_vendor                 = 0;
  uint16_t    midi_product                = 0;
  char        midi_device_name[32]        = {0};

  // Active preset tracker — latched in onPreStateChange() before WLED
  // wipes currentPreset to 0 inside stateUpdated(). The MIDI usermod's
  // paint logic relies on this staying accurate across the wipe.
  byte        tracked_active_preset      = 0;

  // "Is this preset slot actually saved on disk?" — uses WLED's built-in
  // presetCache (built once at boot by buildPresetCache() in wled.cpp:1810,
  // refreshed automatically by doSaveState() at presets.cpp:269 and cleared
  // by deletePreset() at presets.cpp:285). getCachedPresetExists() is O(1)
  // and acquires NO JSON buffer, so it's safe to call from onStateChange
  // without causing the "Locking JSON buffer failed!" contention that
  // getPresetName() produces when called 64 times in a row.

  // Per-pad / per-LED last-sent color cache. Used to dedup OUT traffic —
  // onStateChange compares the computed color against this array and only
  // calls midi_out_queue() when it actually changed. Initialised to 0xFF so
  // the first repaint sends everything once.
  uint8_t     last_pad_color[64]          = {};
  uint8_t     last_pad_status[64]         = {};  // 0x96 = solid, 0x99 = pulse, etc.
  uint8_t     last_scene1_led             = 0xFF;  // Scene 1 (note 112) — toggleMirrorX indicator
  uint8_t     last_scene2_led             = 0xFF;  // Scene 2 (note 113) — toggleReverseX indicator
  uint8_t     last_scene3_led             = 0xFF;  // Scene 3 (note 114) — toggleMirrorY indicator
  uint8_t     last_scene4_led             = 0xFF;  // Scene 4 (note 115) — toggleReverseY indicator
  uint8_t     last_scene5_led             = 0xFF;  // Scene 5 (note 116) — toggleTranspose indicator
  uint8_t     last_scene6_led             = 0xFF;  // Scene 6 (note 117) — default unused (kept for symmetry)
  uint8_t     last_scene7_led             = 0xFF;  // Scene 7 (note 118) — select mode (copy preset)
  uint8_t     last_scene8_led             = 0xFF;  // Scene 8 (note 119) — power
  uint8_t     last_check1_led             = 0xFF;  // Track 1 (note 100) — check1
  uint8_t     last_check2_led             = 0xFF;  // Track 2 (note 101) — check2
  uint8_t     last_check3_led             = 0xFF;  // Track 3 (note 102) — check3
  uint8_t     last_track4_led             = 0xFF;  // Track 4 (note 103) — repaint / reboot-armed
  uint8_t     last_track5_led             = 0xFF;  // Track 5 (note 104) — prev FX (disabled during playlist)
  uint8_t     last_track6_led             = 0xFF;  // Track 6 (note 105) — next FX (disabled during playlist)
  uint8_t     last_track7_led             = 0xFF;  // Track 7 (note 106) — prev preset (disabled during playlist)
  uint8_t     last_track8_led             = 0xFF;  // Track 8 (note 107) — next preset (disabled during playlist)
  uint8_t     last_scene_led              = 0xFF;  // legacy alias

  // 0xFF sentinel for "never sent" — distinct from valid colors 0..127
  static constexpr uint8_t UNSENT = 0xFF;

  // APC Mini MK2 note/CC layout (per Akai v1.0 PDF):
  //   Pads 0..63       (notes 0x00..0x3F)  -> Presets 1..64
  //   Track buttons    (notes 0x64..0x6B)  -> 100..107
  //   Scene launches   (notes 0x70..0x77)  -> 112..119
  //   Shift            (note 0x7A)         -> 122 (no LED)
  //   Faders 1..6      (CC 0x30..0x35)     -> 48..53 (effect params)
  //   Faders 7..8      (CC 0x36..0x37)     -> 54..55 (default unused)
  //   Master fader     (CC 0x38)           -> 56 (global brightness)
  static constexpr uint8_t kNumPads          = 64;
  static constexpr uint8_t kPadsPerRow       = 8;
  static constexpr uint8_t kNumTracks        = 8;
  static constexpr uint8_t kNumScenes        = 8;
  static constexpr uint8_t kNumCCs           = 128;
  static constexpr uint8_t kNoteFirstTrack   = 0x64;
  static constexpr uint8_t kNoteLastTrack    = 0x6B;
  static constexpr uint8_t kNoteFirstScene   = 0x70;
  static constexpr uint8_t kNoteLastScene    = 0x77;
  static constexpr uint8_t kNoteShift        = 0x7A;
  static constexpr uint8_t kCCFaderFirst     = 0x30;
  static constexpr uint8_t kCCFaderLast      = 0x35;
  static constexpr uint8_t kCCFader7         = 0x36;
  static constexpr uint8_t kCCFader8         = 0x37;
  static constexpr uint8_t kCCMasterFader    = 0x38;
  static constexpr uint8_t kActionBufSize    = 16;

  // Throttle for fader-driven websocket pushes. Sweeping a fader would
  // otherwise generate one WS message per CC packet (e.g., 127 messages
  // per sweep). This caps the rate to one push per interval — the GUI
  // shows the final value within the interval.
  uint32_t    lastWsFaderPush            = 0;
  static constexpr uint32_t WS_FADER_PUSH_INTERVAL_MS = 250;

  // FPSTR-friendly config namespace keys. PROGMEM is a no-op on ESP32
  // (see cores/esp32/pgmspace.h), so these can be defined inline as constexpr
  // to avoid ODR violations when the header is included from multiple TUs
  // (usermods_list.cpp + wled.cpp via midi_usb_host.cpp).
  static constexpr const char _name[]    PROGMEM = "MIDI-USB";
  static constexpr const char _key_enabled[] PROGMEM = "enabled";
  static constexpr const char _key_feedback_enabled[] PROGMEM = "feedback_enabled";

  // ---------------------------------------------------------------------------
  // Action dispatcher (private)
  // ---------------------------------------------------------------------------
  // Templated so the size limit is enforced at compile time per call site
  // (matches the kActionBufSize bound on every action buffer).
  template<size_t N>
  static void setAction(char (&dst)[N], const char* src) {
    strncpy(dst, src, N - 1);
    dst[N - 1] = '\0';
  }

  // Wipe every LED and reboot. Used by both the rebootArm action and the
  // shift+fullRepaint action; the 200ms drain keeps the OUT ring from
  // losing the final wipe packets before the CPU halts.
  void executeReboot() {
    reboot_armed = false;
    wipeAllLeds();
    unsigned long drain_start = millis();
    for (int i = 0; i < 200; i++) { midi_usb_poll(); delay(1); }
    USER_PRINTF("[MIDI] reboot drain took %lu ms\n",
                (unsigned long)(millis() - drain_start));
    ESP.restart();
  }

  // Build a human-readable preset name for the save flow: the current
  // effect's display name (FX metadata stripped at '@', trailing
  // whitespace trimmed) followed by `suffix` (" (Pad N)" or " (CC N)").
  // Buffer must be at least 40 bytes (matches the call sites in
  // handleIncomingMidi).
  void buildPresetSaveName(char* out, size_t out_size, const char* suffix) {
    const char* fx_name = strip.getModeData(strip.getMainSegment().mode);
    String fx_clean = strip_unicode(String(fx_name ? fx_name : ""));
    int at_pos = fx_clean.indexOf('@');
    if (at_pos >= 0) fx_clean = fx_clean.substring(0, at_pos);
    while (fx_clean.length() > 0 &&
           isspace((unsigned char)fx_clean[fx_clean.length() - 1])) {
      fx_clean.remove(fx_clean.length() - 1);
    }
    // Reserve room for the suffix + null. snprintf truncates cleanly if
    // the effect name is longer than the available buffer.
    size_t suffix_len = strlen(suffix);
    size_t max_name = (out_size > suffix_len + 1) ? (out_size - suffix_len - 1) : 0;
    snprintf(out, out_size, "%.*s%s", (int)max_name, fx_clean.c_str(), suffix);
  }

  // Apply a verb action (from a track / scene button press, or from a CC whose
  // mapping is a verb rather than a parameter name). value is the optional
  // argument for actions like "save<N>" (currently unused — shift+pad uses
  // savePreset() directly).
  //
  // Dispatch: simple "value → write" and "verb → toggle" actions are
  // resolved via the static kActionHandlers table. Actions whose behaviour
  // depends on `shift_held` (rebootArm, fullRepaint+shift) or whose
  // dispatch is fan-out (toggleCheck1..3, toggleMirrorX..Y/Transpose)
  // stay as inline if-chains below the table lookup.
  using ActionFn = void (MidiUsermod::*)(uint8_t value);

  // Build the dispatch table on first use, inside runAction(). The
  // table can't be a static constexpr class member because the act_*
  // method bodies are defined further down — taking their addresses
  // inline in the table would force forward-declaring every method
  // (and g++ rejects the "declare then inline-define" pattern for
  // static constexpr member tables). A function-local static
  // initializes on first call, when the class is fully defined, so
  // the address-of is well-formed. Built once, no per-call cost.

  void runAction(const char* action, uint8_t value = 0) {
    if (!action || !*action) return;

    // ---- Fader parameter setters (CC-mapped, not user-assignable) ----
    // 7-bit MIDI value → 8-bit WLED global. These are wired to fader
    // CCs in the constructor and don't appear in the GUI dropdown
    // (kGUIVerbs), so they don't go through the dispatch table below.
    if (!strcmp(action, "bri")) {
      bri = cc7_to_wled8(value);
      strip.setBrightness(bri, true);
      stateUpdated(CALL_MODE_BUTTON);
      return;
    }
    if (!strcmp(action, "effectSpeed")) {
      effectSpeed = cc7_to_wled8(value);
      applyValuesToSelectedSegs();
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }
    if (!strcmp(action, "effectIntensity")) {
      effectIntensity = cc7_to_wled8(value);
      applyValuesToSelectedSegs();
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }
    if (!strcmp(action, "effectPalette")) {
      // 0..127 mapped linearly to the palette range, so every CC value
      // drives a UNIQUE palette — no repeats (which `value % N` would
      // cause when palette count < 128) and no skips.
      uint16_t pc = strip.getPaletteCount();
      if (pc == 0) return;
      effectPalette = (uint8_t)((uint16_t)value * pc / 128);
      if (effectPalette >= pc) effectPalette = pc - 1;
      applyValuesToSelectedSegs();
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }
    if (!strcmp(action, "effectCurrent")) {
      effectCurrent = (uint8_t)((value << 1) % strip.getModeCount());
      applyValuesToSelectedSegs();
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }
    if (!strcmp(action, "effectCustom1")) {
      // custom1/custom2 are per-Segment (FX.h:424), not globals.
      strip.getMainSegment().custom1 = cc7_to_wled8(value);
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }
    if (!strcmp(action, "effectCustom2")) {
      strip.getMainSegment().custom2 = cc7_to_wled8(value);
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }
    if (!strcmp(action, "effectCustom3")) {
      // 0..31 — clamp to the range (FX uses this for fine detail where
      // 5 bits of resolution is enough).
      uint8_t v = value;
      if (v > 31) v = 31;
      strip.getMainSegment().custom3 = v;
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }

    // ---- User-assignable verbs (track/scene buttons + GUI dropdown) ----
    // Indexes match kGUIVerbs; nullptr entries are handled inline below.
    //   0  power         10 toggleCheck1
    //   1  blackout      11 toggleCheck2
    //   2  full          12 toggleCheck3
    //   3  nightlight    13 toggleMirrorX
    //   4  nextfx        14 toggleReverseX
    //   5  prevfx        15 toggleMirrorY
    //   6  nextpal       16 toggleReverseY
    //   7  prevpal       17 toggleTranspose
    //   8  nextpreset    18 toggleFreeze
    //   9  prevpreset    19 fullRepaint (inline: shift-gated)
    //                    20 rebootArm   (inline: shift-gated)
    //                    21 selectMode
    static const ActionFn kActionHandlers[kNumGUIVerbs] = {
      /* 0  power         */ &MidiUsermod::act_power,
      /* 1  blackout      */ &MidiUsermod::act_blackout,
      /* 2  full          */ &MidiUsermod::act_full,
      /* 3  nightlight    */ &MidiUsermod::act_nightlight,
      /* 4  nextfx        */ &MidiUsermod::act_nextfx,
      /* 5  prevfx        */ &MidiUsermod::act_prevfx,
      /* 6  nextpal       */ &MidiUsermod::act_nextpal,
      /* 7  prevpal       */ &MidiUsermod::act_prevpal,
      /* 8  nextpreset    */ &MidiUsermod::act_nextpreset,
      /* 9  prevpreset    */ &MidiUsermod::act_prevpreset,
      /* 10 toggleCheck1  */ nullptr,  // inline (fan-out)
      /* 11 toggleCheck2  */ nullptr,
      /* 12 toggleCheck3  */ nullptr,
      /* 13 toggleMirrorX */ nullptr,  // inline (fan-out)
      /* 14 toggleReverseX*/ nullptr,
      /* 15 toggleMirrorY */ nullptr,
      /* 16 toggleReverseY*/ nullptr,
      /* 17 toggleTransp. */ nullptr,
      /* 18 toggleFreeze  */ &MidiUsermod::act_freeze,
      /* 19 fullRepaint   */ nullptr,  // inline (shift-gated)
      /* 20 rebootArm     */ nullptr,  // inline (shift-gated)
      /* 21 selectMode    */ &MidiUsermod::act_selectMode,
    };
    for (uint8_t i = 0; i < kNumGUIVerbs; i++) {
      if (!strcmp(action, kGUIVerbs[i])) {
        if (kActionHandlers[i]) {
          (this->*kActionHandlers[i])(value);
          return;
        }
        break;  // handled inline below
      }
    }

    // ---- Per-effect checkmark toggles (Segment::check1/2/3) ----
    // FX-specific options that some effects like Freqwave / DJ Light /
    // etc. expose. If the current effect doesn't use a given check,
    // toggling it is a harmless no-op. We sync the strip, the state,
    // and the websocket push in that order.
    if (!strcmp(action, "toggleCheck1") ||
        !strcmp(action, "toggleCheck2") ||
        !strcmp(action, "toggleCheck3")) {
      Segment& seg = strip.getMainSegment();
      if      (!strcmp(action, "toggleCheck1")) seg.check1 = !seg.check1;
      else if (!strcmp(action, "toggleCheck2")) seg.check2 = !seg.check2;
      else                                      seg.check3 = !seg.check3;
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }

    // ---- Segment transform toggles (mirror/reverse/transpose) ----
    // Act on `target_segment` (default main segment, index 0). For
    // non-2D segments the Y/transpose flags are harmless no-ops.
    if (!strcmp(action, "toggleMirrorX") ||
        !strcmp(action, "toggleReverseX") ||
        !strcmp(action, "toggleMirrorY") ||
        !strcmp(action, "toggleReverseY") ||
        !strcmp(action, "toggleTranspose")) {
      Segment& seg = strip.getSegment(target_segment);
      if      (!strcmp(action, "toggleMirrorX"))   seg.mirror    = !seg.mirror;
      else if (!strcmp(action, "toggleReverseX"))  seg.reverse   = !seg.reverse;
      else if (!strcmp(action, "toggleMirrorY"))   seg.mirror_y  = !seg.mirror_y;
      else if (!strcmp(action, "toggleReverseY"))  seg.reverse_y = !seg.reverse_y;
      else                                         seg.transpose = !seg.transpose;
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }

    // ---- Shift-gated: rebootArm (only meaningful while shift held) ----
    // First press: arm. Second press (shift STILL held): execute.
    // Releasing shift early cancels (see handleIncomingMidi NoteOff path).
    if (!strcmp(action, "rebootArm") && shift_held) {
      if (!reboot_enabled) {
        USER_PRINTLN(F("[MIDI] rebootArm ignored — reboot_enabled is false"));
        return;
      }
      if (!reboot_armed) {
        reboot_armed = true;
        reboot_armed_ms = millis();
        USER_PRINTLN(F("[MIDI] reboot ARMED — press shift+button again to execute"));
        stateUpdated(CALL_MODE_BUTTON);
      } else {
        USER_PRINTLN(F("[MIDI] reboot EXECUTING — turning off LEDs and restarting"));
        executeReboot();
      }
      return;
    }

    // ---- Shift-gated: fullRepaint ----
    // Plain press: repaint (reset dedup so fall-through re-sends).
    // Shift+press: arm/execute reboot. Releasing shift early cancels.
    if (!strcmp(action, "fullRepaint")) {
      if (shift_held && reboot_enabled) {
        if (!reboot_armed) {
          reboot_armed = true;
          reboot_armed_ms = millis();
          USER_PRINTLN(F("[MIDI] reboot ARMED (via shift+fullRepaint) — press again to execute"));
          stateUpdated(CALL_MODE_BUTTON);
        } else {
          USER_PRINTLN(F("[MIDI] reboot EXECUTING — turning off LEDs and restarting"));
          executeReboot();
        }
        return;
      }
      invalidateAllLedDedup();
      stateUpdated(CALL_MODE_BUTTON);
      return;
    }

    // ---- Direct reboot (no arm) ----
    // Only invoked if the user has explicitly enabled reboot_enabled
    // in the usermod settings — accidental reboots from a stray button
    // press would be very annoying.
    if (!strcmp(action, "reboot")) {
      if (!reboot_enabled) {
        USER_PRINTLN(F("[MIDI] reboot ignored — reboot_enabled is false"));
        return;
      }
      USER_PRINTLN(F("[MIDI] reboot requested via controller — restarting"));
      delay(100);  // let the OUT packet finish flushing
      ESP.restart();
      return;
    }
  }

  // ---- Per-action handler methods (one per kActionHandlers[] entry) ----
  // All take a uint8_t value; methods that don't use it just ignore it.

  // 7-bit MIDI CC → 8-bit WLED global. (value << 1) is the canonical
  // mapping for bri/effectSpeed/effectIntensity/effectCustom1/2.
  static constexpr uint8_t cc7_to_wled8(uint8_t v) { return (uint8_t)(v << 1); }

  void act_power(uint8_t) {
    toggleOnOff();
    stateUpdated(CALL_MODE_BUTTON);
  }
  void act_blackout(uint8_t) {
    bri = 0;
    strip.setBrightness(0, true);
    stateUpdated(CALL_MODE_BUTTON);
  }
  void act_full(uint8_t) {
    bri = 255;
    strip.setBrightness(255, true);
    stateUpdated(CALL_MODE_BUTTON);
  }
  void act_nightlight(uint8_t) {
    nightlightActive = !nightlightActive;
    stateUpdated(CALL_MODE_BUTTON);
  }
  void act_nextfx(uint8_t) {
    // Walk the alphabetically-sorted effect index (matching the web
    // UI listing order) rather than the internal id sequence. The
    // sorted index is built lazily inside the core helper.
    const uint16_t count = getEffectDisplayCount();
    if (count > 0) {
      uint16_t cur_pos = 0;
      for (uint16_t i = 0; i < count; i++) {
        if (getEffectIdByDisplayIndex(i) == effectCurrent) { cur_pos = i; break; }
      }
      uint16_t next_pos = (cur_pos + 1) % count;
      effectCurrent = getEffectIdByDisplayIndex(next_pos);
    } else {
      effectCurrent = (uint8_t)((effectCurrent + 1) % strip.getModeCount());
    }
    applyValuesToSelectedSegs();
    colorUpdated(CALL_MODE_BUTTON);
    wsPushIfDue();
  }
  void act_prevfx(uint8_t) {
    const uint16_t count = getEffectDisplayCount();
    if (count > 0) {
      uint16_t cur_pos = 0;
      for (uint16_t i = 0; i < count; i++) {
        if (getEffectIdByDisplayIndex(i) == effectCurrent) { cur_pos = i; break; }
      }
      uint16_t prev_pos = (cur_pos == 0) ? (count - 1) : (cur_pos - 1);
      effectCurrent = getEffectIdByDisplayIndex(prev_pos);
    } else {
      effectCurrent = (effectCurrent == 0)
          ? (uint8_t)(strip.getModeCount() - 1)
          : (uint8_t)(effectCurrent - 1);
    }
    applyValuesToSelectedSegs();
    colorUpdated(CALL_MODE_BUTTON);
    wsPushIfDue();
  }
  void act_nextpal(uint8_t) {
    effectPalette = (uint8_t)((effectPalette + 1) % strip.getPaletteCount());
    applyValuesToSelectedSegs();
    colorUpdated(CALL_MODE_BUTTON);
  }
  void act_prevpal(uint8_t) {
    effectPalette = (effectPalette == 0)
        ? (uint8_t)(strip.getPaletteCount() - 1)
        : (uint8_t)(effectPalette - 1);
    applyValuesToSelectedSegs();
    colorUpdated(CALL_MODE_BUTTON);
  }
  void act_nextpreset(uint8_t) { jumpPreset(+1); }   // skips empty + playlists
  void act_prevpreset(uint8_t) { jumpPreset(-1); }
  void act_freeze(uint8_t) {
    Segment& seg = strip.getMainSegment();
    seg.freeze = !seg.freeze;
    stateUpdated(CALL_MODE_BUTTON);
  }
  void act_selectMode(uint8_t) {
    // selectMode is a "hold to activate" gesture — driven by Scene 7
    // NoteOn/NoteOff (mirrors how shift works). The action handler is
    // a no-op; the actual state change happens in the MIDI dispatcher
    // (sets select_mode_active = true on NoteOn, false on NoteOff).
  }

  // Push a websocket state update if it's been long enough since the
  // last one. Used for fader-driven state changes so sweeping a fader
  // doesn't spam the GUI with one WS message per CC packet — we push
  // at most every WS_FADER_PUSH_INTERVAL_MS.
  void wsPushIfDue() {
    if (millis() - lastWsFaderPush < WS_FADER_PUSH_INTERVAL_MS) return;
    lastWsFaderPush = millis();
    interfaceUpdateCallMode = CALL_MODE_BUTTON;
    lastInterfaceUpdate = 0;
    updateInterfaces(CALL_MODE_BUTTON);
  }

  // Jump to the next/previous preset, skipping empty slots AND playlists.
// direction is +1 (next) or -1 (prev). Bounded by 1..250 — preset 0 is
// reserved and 251..255 are WLED internals. Playlists are skipped because
// applying one starts a cycling playlist, which isn't what the user wants
// when they press "next preset" — they want a single regular preset.
  void jumpPreset(int direction) {
    if (currentPreset < 1) currentPreset = 1;
    int start = currentPreset;
    for (int i = 0; i < 250; i++) {
      currentPreset += direction;
      if (currentPreset < 1)   currentPreset = 250;
      if (currentPreset > 250) currentPreset = 1;
      if (currentPreset == start) break;  // wrapped the whole list
      // Skip empty slots AND playlists. The presetCache tells us both:
      // exists=true + isPlaylist=true → it's a saved playlist → skip.
      // exists=true + isPlaylist=false → it's a regular preset → land.
      if (getCachedPresetExists(currentPreset)
          && !(presetCache != nullptr && presetCache[currentPreset].isPlaylist)) {
        break;
      }
    }
    if (getCachedPresetExists(currentPreset)
        && !(presetCache != nullptr && presetCache[currentPreset].isPlaylist)) {
      tracked_active_preset = currentPreset;
      applyPreset(currentPreset, CALL_MODE_BUTTON_PRESET);
      handlePresets();
    } else {
      // Nothing to land on (no presets at all, or only playlists) —
      // leave currentPreset as it was.
      currentPreset = start;
    }
  }

  // Copy a preset from src slot to dest slot by editing /presets.json
  // directly. Returns true on success, false on any failure (caller
  // surfaces the failure by flashing the destination pad red while
  // the select key is held). Preserves presetCache[dest] so the
  // on-screen LED updates without requiring a full preset cache rebuild.
  static constexpr const char PRESETS_FILE[] = "/presets.json";

  bool copyPresetToSlot(int src, int dest) {
    if (src <= 0 || src > 250 || dest <= 0 || dest > 250 || src == dest) return false;
    if (getCachedPresetExists(dest)) {
      USER_PRINTLN(F("[MIDI] copyPresetToSlot: destination already exists, no-op"));
      return false;
    }
    if (!requestJSONBufferLock(20)) {
      USER_PRINTLN(F("[MIDI] copyPresetToSlot: JSON buffer busy"));
      return false;
    }
    // Allocate our own JsonDocument for the presets.json content (don't
    // touch WLED's fileDoc — it's owned by the JSON / preset subsystem).
    // 64 KB supports 128+ presets; on the P4 the malloc routes to
    // PSRAM (MALLOC_CAP_SPIRAM) when available, so this doesn't eat
    // internal RAM. The previous 16 KB silently overflowed around the
    // 3rd copy, producing a truncated JSON that WLED's next read
    // couldn't parse.
    DynamicJsonDocument copy_doc(64 * 1024);
    bool ok = readObjectFromFile(PRESETS_FILE, nullptr, &copy_doc);
    if (!ok) {
      releaseJSONBufferLock();
      USER_PRINTLN(F("[MIDI] copyPresetToSlot: read presets.json failed"));
      return false;
    }
    JsonObject root = copy_doc.as<JsonObject>();
    char src_key[4];
    snprintf(src_key, sizeof(src_key), "%d", src);
    if (!root.containsKey(src_key)) {
      releaseJSONBufferLock();
      USER_PRINTF("[MIDI] copyPresetToSlot: source preset %d not in file\n", src);
      return false;
    }
    char dest_key[4];
    snprintf(dest_key, sizeof(dest_key), "%d", dest);
    root.remove(dest_key);  // defensive; caller checked but be safe
    // Copy the source subtree directly into the dest subtree in the
    // SAME document. The previous version serialized to a String
    // and deserialized into a separate 8KB doc, which silently
    // dropped the "playlist" sub-object on large playlists (the
    // 8KB src_only doc overflowed) � so copying a playlist produced
    // a regular preset instead of a playlist. Copying in-document
    // preserves every nested object/array without a memory ceiling.
    JsonObject src_obj = root[src_key].as<JsonObject>();
    JsonObject dest_obj = root.createNestedObject(dest_key);
    // Copy all top-level fields including the "playlist" sub-object
    // (which itself contains ps/dur/transition arrays).
    uint16_t kv_count = 0;
    for (JsonPair kv : src_obj) {
      dest_obj[kv.key().c_str()] = kv.value();
      kv_count++;
    }
    // WLEDMM: copy all top-level fields, count them only for debug.

    // Write the file ourselves. writeObjectToFile() can't be used here:
    // it calls bufferedFind(key), and passing key=nullptr dereferences
    // a null pointer inside strlen() at file.cpp:65. So we serialize
    // copy_doc to a String and overwrite the file with it.
    String full_json;
    size_t serialized_len = serializeJson(copy_doc, full_json);
    releaseJSONBufferLock();
    bool write_ok = false;
    {
      File wf = WLED_FS.open(PRESETS_FILE, "w");
      if (!wf) {
        USER_PRINTLN(F("[MIDI] copyPresetToSlot: failed to open presets.json for write"));
        return false;
      }
      size_t written = wf.print(full_json);
      wf.close();
      // Verify the write by re-reading the destination preset.
      // Without this, a truncated/overwritten file would leave the
      // cache saying the preset exists while the file says it
      // doesn't, and the next load attempt says "preset does not
      // exist" while the LED is stuck lit. Verify and skip the cache
      // update if the destination didn't actually make it into the
      // file.
      DynamicJsonDocument verify_doc(8 * 1024);
      char dest_key[4];
      snprintf(dest_key, sizeof(dest_key), "%d", dest);
      if (readObjectFromFileUsingId(PRESETS_FILE, dest, &verify_doc)) {
        write_ok = true;
      }
    }
    if (!write_ok) {
      // File write did not produce a readable destination — bailing.
      return false;
    }
    updateFSInfo();

    // Mark the preset file as modified so the GUI's preset list refreshes.
    presetsModifiedTime = toki.second();

    if (presetCache != nullptr) {
      // Mirror what doSaveState() does on a real save so the LED feedback
      // shows the new pad as saved immediately. Match WLED's own detection
      // (presets.cpp:361) — `!presetObj["playlist"].isNull()`. The
      // "playlist" key is stored as a JsonObject containing the ps/dur/
      // transition arrays (see playlist.cpp:167 serializePlaylist), so
      // checking isNull() is the correct gate; is<JsonArray>() would miss
      // it and the destination pad would paint blue instead of magenta.
      presetCache[dest].exists = true;
      presetCache[dest].isPlaylist = !src_obj[F("playlist")].isNull();
      // Propagate the repeat value so the pad paints as finite (32) or
      // infinite (53) according to the source. Without this, getPresetRepeat
      // would return 0 for the destination and the pad would always paint
      // as infinite (53) regardless of the source's actual repeat value.
      if (presetCache[dest].isPlaylist) {
        JsonObject pl = src_obj[F("playlist")];
        presetCache[dest].repeat = (uint8_t)(pl[F("repeat")].as<int>() | 0);
      } else {
        presetCache[dest].repeat = 0;
      }
      String nm;
      if (src_obj["n"]) {
        nm = (const char*)(src_obj["n"]);
      }
      strlcpy(presetCache[dest].name, nm.c_str(), sizeof(presetCache[dest].name));
    }
    stateUpdated(CALL_MODE_BUTTON_PRESET);
    // The onEvent(PresetListMutated) handler in this class forces
    // the WS push with cooldown bypass when core's doSaveState() /
    // writeObjectToFileUsingId() publish the event — no need to call
    // updateInterfaces() here.
    USER_PRINTF("[MIDI] copyPresetToSlot: %d -> %d OK\n", src, dest);
    return true;
  }

 public:
  // Wipe every per-LED "last sent" dedup slot back to UNSENT, so the next
  // paint loop re-sends every LED instead of deduping. Used by the
  // fullRepaint action and the power-on transition. Cheaper than
  // wipeAllLeds() (no midi_out_queue traffic) and works for pads AND
  // single-color buttons in one call.
  //
  // Public so the v3 USB host task (midi_usb_host.cpp, a separate
  // TU) can call this from its repaint task. The onEvent
  // (UsbDeviceChanged) handler also calls this internally.
  void invalidateAllLedDedup() {
    memset(last_pad_color,  UNSENT, sizeof(last_pad_color));
    memset(last_pad_status, UNSENT, sizeof(last_pad_status));
    last_scene1_led  = UNSENT;
    last_scene2_led  = UNSENT;
    last_scene3_led  = UNSENT;
    last_scene4_led  = UNSENT;
    last_scene5_led  = UNSENT;
    last_scene6_led  = UNSENT;
    last_scene7_led  = UNSENT;
    last_scene8_led  = UNSENT;
    last_check1_led  = UNSENT;
    last_check2_led  = UNSENT;
    last_check3_led  = UNSENT;
    last_track4_led  = UNSENT;
    last_track5_led  = UNSENT;
    last_track6_led  = UNSENT;
    last_track7_led  = UNSENT;
    last_track8_led  = UNSENT;
  }

 public:
  // ---------------------------------------------------------------------------
  // Construction / lifecycle
  // ---------------------------------------------------------------------------
  MidiUsermod(const char* name = "MIDI-USB", bool enabled = true)
      : Usermod(name, enabled) {
    // Pad layout: top-left = preset 1, left-to-right then top-to-bottom.
    // The APC Mini MK2 sends notes 0..63 with note 0 = bottom-left and
    // note 56..63 = top row. Flip the row index so that the physical
    // top-left pad (note 56) maps to preset 1, top-right (note 63) to
    // preset 8, next row down to 9..16, etc., ending with bottom-left
    // (note 0) = preset 57 and bottom-right (note 7) = preset 64.
    for (int i = 0; i < kNumPads; i++) {
      uint8_t row_from_bottom = i / kPadsPerRow;       // 0..7
      uint8_t col            = i % kPadsPerRow;       // 0..7
      uint8_t row_from_top   = (kPadsPerRow - 1) - row_from_bottom;
      pad_to_preset[i] = (int8_t)(row_from_top * kPadsPerRow + col + 1);
    }

    // Track buttons:
    //   1, 2, 3 = Segment.check1/2/3 (FX-specific options, no-op if unused)
    //   4       = full controller repaint (shift+ = arm/execute reboot)
    //   5       = previous effect (shift+ = previous palette)
    //   6       = next effect     (shift+ = next palette)
    //   7, 8    = previous/next preset (skips empty + finite playlists)
    // Tracks 4..8 all light red when enabled (no playlist running); the
    // tracks 5..8 group goes dark when a playlist is active (you don't
    // navigate presets while a playlist is driving the show).
    setAction(track_button_action[0], "toggleCheck1");
    setAction(track_button_action[1], "toggleCheck2");
    setAction(track_button_action[2], "toggleCheck3");
    setAction(track_button_action[3], "fullRepaint");
    setAction(track_button_action[4], "prevfx");
    setAction(track_button_action[5], "nextfx");
    setAction(track_button_action[6], "prevpreset");
    setAction(track_button_action[7], "nextpreset");

    // Scene launches:
    //   1 = toggleMirrorX
    //   2 = toggleReverseX
    //   3 = toggleMirrorY
    //   4 = toggleReverseY
    //   5 = toggleTranspose
    //   6 = unused
    //   7 = "Select" — enters select mode for preset copy flow
    //   8 = power on/off toggle
    // (toggleFreeze is implemented in runAction() but not assigned to a
    // default scene button; assign it via cfg.json if you want it.)
    setAction(scene_button_action[0], "toggleMirrorX");
    setAction(scene_button_action[1], "toggleReverseX");
    setAction(scene_button_action[2], "toggleMirrorY");
    setAction(scene_button_action[3], "toggleReverseY");
    setAction(scene_button_action[4], "toggleTranspose");
    setAction(scene_button_action[5], "");  // Scene 6 unused
    setAction(scene_button_action[6], "selectMode");
    setAction(scene_button_action[7], "power");

    // Shift+button actions. Default assignments preserve the user's
    // intended behavior:
    //   Tracks 1..3: shift does the same as plain (toggleCheck1/2/3)
    //   Track 4:     shift triggers reboot arm via fullRepaint+shift
    //   Tracks 5..8: shift = palette/preset navigation
    //   Scenes 1..8: empty (no default shift behavior)
    setAction(track_button_shift_action[0], "toggleCheck1");
    setAction(track_button_shift_action[1], "toggleCheck2");
    setAction(track_button_shift_action[2], "toggleCheck3");
    setAction(track_button_shift_action[3], "fullRepaint");  // shift = reboot arm
    setAction(track_button_shift_action[4], "prevpal");
    setAction(track_button_shift_action[5], "nextpal");
    setAction(track_button_shift_action[6], "prevpreset");
    setAction(track_button_shift_action[7], "nextpreset");
    for (int i = 0; i < 8; i++) setAction(scene_button_shift_action[i], "");

    // Clear all CC actions.
    for (int i = 0; i < kNumCCs; i++) cc_to_action[i][0] = '\0';

    // Fader 9 (CC kCCMasterFader) -> Global brightness.
    setAction(cc_to_action[kCCMasterFader], "bri");
    // Faders 1..6 (CC kCCFaderFirst..kCCFaderLast) -> Effect parameters:
    //   1 = speed    (0..255)
    //   2 = intensity (0..255)
    //   3 = custom1  (0..255)
    //   4 = custom2  (0..255)
    //   5 = custom3  (0..31, clamped — FX uses this for fine detail)
    //   6 = palette  (0..127 direct, modulo palette count)
    setAction(cc_to_action[kCCFaderFirst + 0], "effectSpeed");
    setAction(cc_to_action[kCCFaderFirst + 1], "effectIntensity");
    setAction(cc_to_action[kCCFaderFirst + 2], "effectCustom1");
    setAction(cc_to_action[kCCFaderFirst + 3], "effectCustom2");
    setAction(cc_to_action[kCCFaderFirst + 4], "effectCustom3");
    setAction(cc_to_action[kCCFaderFirst + 5], "effectPalette");
    // Faders 7..8 (CC kCCFader7, kCCFader8) default unused.

    // All CCs start "not taken over" — first CC after connect must wait
    // until the fader is near the current WLED value before driving it.
    memset(cc_taken_over, 0, sizeof(cc_taken_over));
  }

  void setup() override {
    USER_PRINTLN(F("[MIDI] usermod_v3_midi loaded"));
    initDone = true;
  }

  void loop() override {
    if (!enabled || strip.isUpdating()) return;

    // Watch for a non-looping playlist ending WITHOUT a playlistEndPreset.
    // handlePlaylist() in wled00/playlist.cpp calls unloadPlaylist() and
    // then applyPreset(playlistEndPreset) only if playlistEndPreset > 0.
    // If it's 0, the playlist stops but no stateUpdated fires — so our
    // onStateChange doesn't get a chance to revert the parent pad's
    // pulse (0x99) to solid magenta or turn the last child's fast
    // blink (0x9B) into solid green. The controller stays in the
    // "playlist playing" visual state forever.
    //
    // We catch this here: if currentPlaylist just transitioned from
    // positive to -1, force a repaint. Tracked on every loop iteration
    // (cheap — just one comparison + one assignment).
    static int8_t prev_currentPlaylist = -2;  // sentinel: uninitialized
    if (prev_currentPlaylist > 0 && currentPlaylist < 0) {
      // Playlist just stopped with no end preset (or end preset was
      // 0). Clear tracked_active_preset so the formerly-playing child
      // pad doesn't stay green, and let the paint loop re-render the
      // parent pad back to solid magenta.
      tracked_active_preset = 0;
      // Fire stateUpdated to trigger onStateChange, which will then
      // re-paint the controller with the post-playlist state.
      stateUpdated(CALL_MODE_BUTTON);
    }
    prev_currentPlaylist = currentPlaylist;
  }

  // Override the v3 hooks added to the Usermod base class.
  //   * onPreStateChange: latch the pre-wipe value of currentPreset
  //     (so tracked_active_preset survives the stateUpdated wipe).
  //   * onEvent: react to targeted events from the v3 event bus.
  void onPreStateChange(uint8_t mode) override {
    // Latch the current preset before stateUpdated() wipes it to 0.
    // Without this, onStateChange() (called later in the same
    // stateUpdated() flow) sees currentPreset == 0 even when we just
    // applied a preset via applyPreset(). The MIDI usermod's paint
    // logic relies on tracked_active_preset staying accurate across
    // the wipe.
    if (currentPreset != 0) tracked_active_preset = currentPreset;
  }

  void onEvent(const wled::Event& ev) override {
    // React to targeted events from the v3 event bus. The USB host
    // publishes UsbDeviceChanged from wled.cpp; core's presets.cpp
    // publishes PresetListMutated after doSaveState() / deletePreset();
    // core's playlist.cpp publishes PlaylistEnded when a playlist stops.
    if (ev.type == wled::EventType::UsbDeviceChanged) {
      midi_connected     = ev.payload.usbDevice.connected;
      midi_vendor        = ev.payload.usbDevice.vid;
      midi_product       = ev.payload.usbDevice.pid;
      if (ev.payload.usbDevice.name[0]) {
        strncpy(midi_device_name, ev.payload.usbDevice.name,
                sizeof(midi_device_name) - 1);
        midi_device_name[sizeof(midi_device_name) - 1] = '\0';
      } else {
        midi_device_name[0] = '\0';
      }
      if (!midi_connected) {
        // Device disconnected — reset soft-takeover state so the next
        // device connection starts fresh (the user may have changed
        // WLED values via the GUI while we were disconnected, so the
        // stale "taken over" flags would now drive WLED incorrectly).
        memset(cc_taken_over, 0, sizeof(cc_taken_over));
        select_mode_active = false;
        select_source_pad = 0xFF;
      } else {
        // Force a full repaint on (re)connect so the controller
        // re-paints the new active preset and any on-state LED
        // indicators. This is what the v2 requestFullRepaint() callback
        // used to do, now driven by the connect event.
        invalidateAllLedDedup();
        onStateChange(CALL_MODE_BUTTON);
      }
      return;
    }
    if (ev.type == wled::EventType::PresetListMutated) {
      // Core's doSaveState() and deletePreset() publish this event
      // after mutating /presets.json. Bypass the 1.2s interface-update
      // cooldown and force a WS push so the GUI sees the new
      // preset list immediately. updateInterfaces() reads
      // interfaceUpdateCallMode to know which mode to broadcast.
      interfaceUpdateCallMode = (uint8_t)wled::EventType::PresetListMutated;
      lastInterfaceUpdate = 0;
      updateInterfaces((uint8_t)wled::EventType::PresetListMutated);
      return;
    }
    if (ev.type == wled::EventType::PlaylistEnded) {
      // The loop() watch above still catches the rare no-end-preset
      // case; this event-driven path is the more reliable one for the
      // common playlist-stop case.
      tracked_active_preset = 0;
      stateUpdated(CALL_MODE_BUTTON);
    }
  }

  // ---------------------------------------------------------------------------
  // State-change feedback: repaint pads using the Madrix palette.
  //   no light  = no preset mapped/saved
  //   blue      = preset saved, not active
  //   green     = active preset (no playlist)
  //   magenta   = active preset (playlist active)
  //
  // Called by WLED core after every state mutation. Repaints all
  // 64 RGB pads and 16 single-color buttons (Track 1-8 + Scene 1-8).
  // Each color is compared against the per-LED dedup slot and only
  // sent when it actually changed — this cuts bulk-OUT traffic from
  // 64+ packets per state change to just the LEDs that actually
  // need updating.
  // ---------------------------------------------------------------------------
  void onStateChange(uint8_t mode) override {
    if (!enabled || !midi_connected || !feedback_enabled) return;
    if (mode == CALL_MODE_INIT || mode == CALL_MODE_NO_NOTIFY) return;

    USER_PRINTF("[MIDI] onStateChange mode=%u currentPreset=%u tracked=%u\n",
                (unsigned)mode, (unsigned)currentPreset,
                (unsigned)tracked_active_preset);
    if (handlePowerPaint()) return;
    paintPads();
    paintSingleColorButtons();   // also calls disarmExpiredRebootArm() internally
  }

  // tracked_active_preset is set ONCE in onPreStateChange() before
  // stateUpdated() wipes currentPreset to 0. See onPreStateChange()
  // above for the latch.

  // Power-state paint. Owns the prev_power_off static so it's
  // updated on every onStateChange call (without that, off→on
  // transitions would never be detected).
  // were never detected).
  //
  // Returns true if power-off is being painted (caller should skip
  // the normal pad-color logic). Otherwise returns false and
  // applies any off→on transition paint.
  bool handlePowerPaint() {
    static bool prev_power_off = false;
    bool power_off = (bri == 0);
    // Always update the static so the off→on transition is reliably
    // detected next time.
    bool transition = (power_off != prev_power_off);
    prev_power_off = power_off;

    if (power_off) {
      // POWER OFF pattern: ALL pads off, ALL track buttons off, ALL
      // scene buttons off EXCEPT scene 8 (the power button itself),
      // which blinks to indicate the device is in standby.
      //
      // We touch every LED every paint so that any state that may
      // have drifted (a new pad color from a preset save, a track
      // that wasn't cleared by wipeAllLeds, etc.) is reliably
      // turned off — and scene 8 reliably blinks. The dedup
      // arrays are reset on the off→on transition (below), so the
      // next paint will re-send everything that matters.
      for (int i = 0; i < kNumPads; i++) {
        if (last_pad_color[i] != 0 || last_pad_status[i] != 0x96) {
          midi_out_queue(0x96, (uint8_t)i, 0);  // solid off
          last_pad_color[i] = 0;
          last_pad_status[i] = 0x96;
        }
      }
      for (uint8_t n = kNoteFirstTrack; n <= kNoteLastTrack; n++) {
        // Just send off for every track button. We don't track
        // last values for tracks here (they're single-color
        // buttons whose velocity encodes color, not RGB pads), so
        // we rely on the state_updated chain to also push them
        // when needed.
        midi_out_queue(0x90, n, 0);
      }
      for (uint8_t n = kNoteFirstScene; n <= kNoteLastScene; n++) {
        uint8_t v = (n == kNoteLastScene) ? 2 : 0;  // scene 8 blinks
        midi_out_queue(0x90, n, v);
      }
      return true;  // signal to caller: skip normal paint
    }

    if (transition) {
      // Just powered on. Reset dedup so the fall-through paint
      // loop repaints the entire controller fresh. We do NOT call
      // wipeAllLeds() here because wiping takes 80 slots in the
      // OUT ring buffer; the dedup reset forces the fall-through
      // to send the correct color for every LED.
      invalidateAllLedDedup();
      // Explicit scene 8 (power button) so it lands solid on
      // immediately, in case the fall-through paint loop is
      // delayed for any reason.
      midi_out_queue(0x90, kNoteLastScene, 1);  // solid on (scene 8)
      last_scene8_led = 1;
    }
    return false;
  }

  // Paint the 64 RGB pads. The status byte (0x96/0x99/0x9B) drives
  // the animation: solid, slow pulse, or fast blink. Color is
  // per-category: yellow for music playlists, red for finite
  // playlists, magenta for looping playlists, green for the active
  // regular preset, blue for any other saved preset, off otherwise.
  void paintPads() {
    // Disarm any expired delete arm BEFORE painting, so this cycle's
    // render reflects the disarmed state and the pad reverts to its
    // normal color immediately (rather than flashing red for one extra
    // cycle after the timeout).
    disarmExpiredDeleteArm();

    const bool playlist_active = (currentPlaylist > 0);
    const byte active = tracked_active_preset;
    const byte playlist_parent = playlist_active ? (byte)currentPlaylist : 0;

    AutoPlaylistState aps = queryAutoPlaylist();
    const uint8_t music_playlist_preset = aps.musicPlaylist;  // 0 if unset
    // Per-preset playlist repeat is read directly from the WLED
    // core's preset cache (presetCache[].repeat, populated in
    // buildPresetCache). No usermod-side cache needed anymore —
    // getPresetRepeat() is O(1) and always reads the latest value.

    // Pad state precedence (first match wins):
    //   1. Unmapped pad               → off
    //   2. Music playlist pad         → yellow (13) in all states
    //   3. Finite playlist pad        → red (32) in all states
    //   4. Looping playlist pad       → magenta (53) in all states
    //   5. Active regular preset      → green (21) solid
    //   6. Saved regular preset       → blue (45) solid
    //   7. Mapped but not saved       → off
    //
    // Within a category, the animation byte (0x96/0x99/0x9B)
    // distinguishes solid / parent-playing-slow-pulse /
    // child-playing-fast-blink.
    auto paint_pad = [&](int8_t preset) -> std::pair<uint8_t, uint8_t> {
      if (preset <= 0) return {0, 0x96};

      const bool is_music = (music_playlist_preset > 0
                             && (uint8_t)preset == music_playlist_preset);
      const bool is_playlist = (presetCache != nullptr
                                && preset <= 250
                                && presetCache[preset].exists
                                && presetCache[preset].isPlaylist);

      // Music playlist (yellow in all states). Music takes priority
      // over regular playlist coloring.
      if (is_music) {
        if (playlist_active && (uint8_t)preset == playlist_parent) return {13, 0x99};
        if (playlist_active && (uint8_t)preset == active)        return {13, 0x9B};
        return {13, 0x96};
      }

      // Playlist parent: a saved playlist preset that's currently playing.
      if (is_playlist && playlist_active
          && (uint8_t)preset == playlist_parent) {
        const bool finite = (getPresetRepeat(preset) > 0);
        return {finite ? uint8_t(32) : uint8_t(53), 0x99};
      }

      // Playlist child: a playlist is running and this preset is the
      // currently-playing child. The child itself is usually NOT a
      // playlist preset (it's a regular preset in the playlist's
      // child list), so this check deliberately does NOT require
      // `is_playlist`. Pick the parent's color category so the child
      // blends with the parent.
      if (playlist_active && (uint8_t)preset == active
          && (uint8_t)preset != playlist_parent) {
        if (music_playlist_preset > 0
            && playlist_parent == music_playlist_preset) {
          return {13, 0x9B};
        }
        const bool parent_finite = (playlist_parent > 0
                                  && playlist_parent <= 250
                                  && getPresetRepeat(playlist_parent) > 0);
        return {parent_finite ? uint8_t(32) : uint8_t(53), 0x9B};
      }

      // Playlist idle: a saved playlist that's NOT currently playing.
      if (is_playlist) {
        const bool finite = (getPresetRepeat(preset) > 0);
        return {finite ? uint8_t(32) : uint8_t(53), 0x96};
      }

      // Regular saved preset.
      if ((uint8_t)preset == active && getCachedPresetExists(preset)) return {21, 0x96};
      if (getCachedPresetExists(preset)) return {45, 0x96};
      return {0, 0x96};
    };

    for (int i = 0; i < kNumPads; i++) {
      const int8_t preset = pad_to_preset[i];
      auto [color, status] = paint_pad(preset);
      // Two visual overrides take precedence over the normal category
      // paint:
      //   * delete_armed_pad: the pad is flashing red-fast-blink to
      //     ask the user to confirm a destructive delete. Cleared on
      //     shift release, timeout, or successful second press.
      //   * copy_failed_pad: the destination pad is flashing
      //     red-fast-blink to signal a copy failure. Cleared on Scene 7
      //     release.
      // Status 0x9B is the fast-blink used by playlist children;
      // color 5 is plain red in the APC Mini MK2 palette.
      if (i == delete_armed_pad || i == copy_failed_pad) {
        color  = 5;
        status = 0x9B;
      }
      // A switch from solid to pulse (e.g., when a playlist starts
      // and the parent should pulse) needs to be sent even if the
      // color didn't change.
      if (color != last_pad_color[i] || status != last_pad_status[i]) {
        midi_out_queue(status, (uint8_t)i, color);
        last_pad_color[i] = color;
        last_pad_status[i] = status;
      }
    }
  }

  // Paint all 16 single-color buttons (Track 1-8 + Scene 1-8). The
  // LED feedback is derived from each button's *assigned action*, not
  // its physical position — so if the user re-assigns Track 5 to
  // toggleMirrorX (or any other action), the LED automatically
  // reflects that new action's state.
  //
  // APC Mini MK2 single-LED protocol: status 0x90 only. Velocity
  // determines behavior:
  //   0x00 = off
  //   0x02 = blink (built-in blink rate; color fixed by APC)
  //   0x01 / 0x03..0x7F = solid on (color by velocity table)
  // The 0x96/0x99/0x9B status bytes are silently dropped, so the
  // only way to get a non-solid indicator is velocity 0x02.
  void paintSingleColorButtons() {
    const bool playlist_active = (currentPlaylist > 0);
    Segment& seg_for_leds = strip.getMainSegment();

    // Action → LED color mapping. Per-action state comes from
    // Segment::checkN (per-effect toggles), Segment::mirror/reverse/
    // transpose/freeze (segment transforms), `bri` (power), or
    // `playlist_active` (navigation actions darken during a playlist).
    // The selectMode/fullRepaint blink overrides are applied per-button
    // in paintSingleColorButton() below.
    auto action_led = [&](const char* action) -> uint8_t {
      if (!action || !action[0]) return 0;
      if (!strcmp(action, "toggleCheck1"))    return seg_for_leds.check1 ? 1 : 0;
      if (!strcmp(action, "toggleCheck2"))    return seg_for_leds.check2 ? 1 : 0;
      if (!strcmp(action, "toggleCheck3"))    return seg_for_leds.check3 ? 1 : 0;
      if (!strcmp(action, "toggleMirrorX"))   return seg_for_leds.mirror    ? 1 : 0;
      if (!strcmp(action, "toggleReverseX"))  return seg_for_leds.reverse   ? 1 : 0;
      if (!strcmp(action, "toggleMirrorY"))   return seg_for_leds.mirror_y  ? 1 : 0;
      if (!strcmp(action, "toggleReverseY"))  return seg_for_leds.reverse_y ? 1 : 0;
      if (!strcmp(action, "toggleTranspose")) return seg_for_leds.transpose ? 1 : 0;
      if (!strcmp(action, "toggleFreeze"))    return seg_for_leds.freeze    ? 1 : 0;
      if (!strcmp(action, "power"))           return (bri > 0) ? 1 : 0;
      if (!strcmp(action, "rebootArm"))       return 1;
      if (!strcmp(action, "nextfx") || !strcmp(action, "prevfx") ||
          !strcmp(action, "nextpal") || !strcmp(action, "prevpal") ||
          !strcmp(action, "nextpreset") || !strcmp(action, "prevpreset")) {
        return playlist_active ? 0 : 1;
      }
      return 0;
    };

    // Per-button painter. Two blink overrides:
    //   * `reboot_armed` while this button's action is rebootArm or
    //     fullRepaint → blink (the "press again to confirm" prompt).
    //   * `select_mode_active` while this button's action is selectMode
    //     → blink (the "press two pads to copy" prompt).
    auto paintSingleColorButton = [&](uint8_t idx, uint8_t& dedup,
                                      uint8_t note_base,
                                      const char* plain_act,
                                      const char* shift_act) {
      const char* act = (shift_act && shift_act[0]) ? shift_act : plain_act;
      const bool armed = (reboot_armed &&
                          (!strcmp(act, "rebootArm") ||
                           !strcmp(act, "fullRepaint")))
                      || (select_mode_active &&
                          !strcmp(act, "selectMode"));
      uint8_t led = armed ? 0x02 : action_led(act);
      if (led != dedup) {
        midi_out_queue(0x90, (uint8_t)(note_base + idx), led);
        dedup = led;
      }
    };

    // Track buttons 1..8 (notes kNoteFirstTrack..+7). Tracks 1..3 use
    // last_checkN_led as the dedup slot; tracks 4..8 use
    // last_trackN_led. Mirrors the member-field naming.
    uint8_t* const track_dedup[kNumTracks] = {
      &last_check1_led, &last_check2_led, &last_check3_led,
      &last_track4_led, &last_track5_led, &last_track6_led,
      &last_track7_led, &last_track8_led,
    };
    for (uint8_t i = 0; i < kNumTracks; i++) {
      paintSingleColorButton(i, *track_dedup[i], kNoteFirstTrack,
                             track_button_action[i],
                             track_button_shift_action[i]);
    }

    // Disarm the reboot arm state if it has timed out BEFORE painting
    // scene buttons — scene buttons often hold the rebootArm action,
    // and we want them to see the disarmed state in this same paint.
    // (Track buttons keep their pre-disarm state for this paint; the
    // next paint will reflect the new value. Matches the original
    // onStateChange ordering where the disarm sat between the two
    // button loops.)
    disarmExpiredRebootArm();

    // Scene launches 1..8 (notes kNoteFirstScene..+7). Same
    // paintSingleColorButton() helper.
    uint8_t* const scene_dedup[kNumScenes] = {
      &last_scene1_led, &last_scene2_led, &last_scene3_led, &last_scene4_led,
      &last_scene5_led, &last_scene6_led, &last_scene7_led, &last_scene8_led,
    };
    for (uint8_t i = 0; i < kNumScenes; i++) {
      paintSingleColorButton(i, *scene_dedup[i], kNoteFirstScene,
                             scene_button_action[i],
                             scene_button_shift_action[i]);
    }
  }

  // Disarm the reboot arm state if the second press didn't arrive
  // within REBOOT_ARM_TIMEOUT_MS. The fall-through paint loop will
  // then re-render the affected button to its non-armed color.
  void disarmExpiredRebootArm() {
    if (reboot_armed && (millis() - reboot_armed_ms) > REBOOT_ARM_TIMEOUT_MS) {
      reboot_armed = false;
    }
  }

  // Disarm the delete arm state if the second press didn't arrive
  // within DELETE_ARM_TIMEOUT_MS. The fall-through paint loop in
  // paintPads() will then re-render the affected pad to its
  // non-armed color. Mirrors disarmExpiredRebootArm() — called at
  // the start of paintPads() so the disarm happens before this
  // cycle's paint.
  void disarmExpiredDeleteArm() {
    if (delete_armed_pad >= 0
        && (millis() - delete_armed_ms) > DELETE_ARM_TIMEOUT_MS) {
      delete_armed_pad = -1;
    }
  }

  // ---------------------------------------------------------------------------
  // Public hooks called from wled.cpp's app_queue drain
  // ---------------------------------------------------------------------------

  // Inbound MIDI packet from the USB Host client. status is the channel-stripped
  // command byte (0x80 NoteOff, 0x90 NoteOn, 0xB0 CC).
  void handleIncomingMidi(uint8_t status, uint8_t d1, uint8_t d2) {
    if (!enabled) return;

    // Per-packet trace — gated by WLED_DEBUG so the log isn't spammed while
    // you're just running. Lets you confirm "USB Host delivered to usermod"
    // end-to-end.
    USER_PRINTF("[MIDI] usermod IN: status=0x%02X d1=%u d2=%u\n", status, d1, d2);

    if (status == 0x90) {                // NoteOn
      if (d2 == 0) {                     // velocity 0 = NoteOff
        if (d1 == kNoteShift) {
          shift_held = false;
          // Track 9 (shift) has no LED per the APC Mini MK2 docs, so no
          // visual feedback to clear. Just update internal state.
          //
          // Releasing shift mid-arm cancels BOTH the reboot arm and
          // the delete arm so their LEDs revert to solid on the next
          // paint. The shift+Track 4 execute path only runs while
          // shift is held, so this guarantees we can't accidentally
          // reboot by releasing shift between the arm and execute.
          // Same logic for the two-press delete confirmation.
          bool repaint = false;
          if (reboot_armed) {
            reboot_armed = false;
            repaint = true;
          }
          if (delete_armed_pad >= 0) {
            delete_armed_pad = -1;
            repaint = true;
          }
          if (repaint) stateUpdated(CALL_MODE_BUTTON);
        }
        // Some controllers send a 0-velocity NoteOn instead of a true
        // NoteOff. Mirror the NoteOff select-mode cleanup here too.
        if (d1 == kNoteFirstScene + 6) {
          select_mode_active = false;
        }
        return;
      }
      if (d1 == kNoteShift) {            // Shift button (Track 9)
        shift_held = true;
        // Track 9 (shift) has no LED per the APC Mini MK2 docs, so no
        // visual feedback to send. Just update internal state; the
        // shift modifier is applied to subsequent track/scene presses.
        return;
      }
      // select mode (Scene 7 with selectMode action) is a "hold to
      // activate" gesture, mirroring how shift works. The action
      // handler is a no-op; the state is driven here on NoteOn (set
      // active) and cleared in the NoteOff path below.
      if (d1 == kNoteFirstScene + 6) {   // Scene 7 (kNoteFirstScene=0x70, +6=0x76)
        if (copy_enabled) {
          select_mode_active = true;
          select_source_pad  = 0xFF;  // no source selected yet
        }
        return;
      }
      if (d1 < kNumPads) {               // Pad 0..63
        int8_t preset = pad_to_preset[d1];
        if (preset <= 0) {
          // Unmapped pad (pad_to_preset[d1] <= 0 — either no preset
          // assigned to this pad, or the slot was explicitly cleared via
          // config). Treat as "stop": if a playlist is active, unload
          // it. This lets the user press any unmapped pad to silence
          // a running playlist without having to navigate to a known
          // preset slot.
          if (currentPlaylist >= 0) {
            unloadPlaylist();
            tracked_active_preset = 0;
            stateUpdated(CALL_MODE_BUTTON_PRESET);
          }
          return;
        }

        // --- Select mode: Scene 5 pressed, waiting for source then dest ---
        if (select_mode_active) {
          if (select_source_pad == 0xFF) {
            // First pad = source.
            select_source_pad = (uint8_t)d1;
            // Clear any stale copy-failure flash from a previous attempt.
            // Otherwise the previous destination's red flash would persist
            // visually (copy_failed_pad was only cleared on Scene 7 release,
            // which the user may not have done between attempts).
            if (copy_failed_pad >= 0) {
              copy_failed_pad = -1;
              stateUpdated(CALL_MODE_BUTTON);
            }
          } else if ((uint8_t)d1 != select_source_pad) {
            // Second pad = destination. Copy preset if destination is empty.
            //
            // On any failure (copy disabled, unmapped dest, dest already
            // occupied, or internal copyPresetToSlot failure) we keep
            // select_mode_active = true so the user can try again, and
            // stash the failed pad in copy_failed_pad so paintPads can
            // flash it red-fast-blink while the select key is held.
            if (!copy_enabled) {
              copy_failed_pad = (int8_t)d1;
              stateUpdated(CALL_MODE_BUTTON);
            } else {
              int8_t dest_preset = pad_to_preset[d1];
              if (dest_preset <= 0 || getCachedPresetExists(dest_preset)) {
                // Destination unmapped or already occupied — flash the
                // destination pad red while the select key is held.
                copy_failed_pad = (int8_t)d1;
                select_source_pad = 0xFF;
                stateUpdated(CALL_MODE_BUTTON);
              } else {
                // Attempt the copy. Source is still needed for the call.
                int8_t src_preset = pad_to_preset[select_source_pad];
                bool ok = copyPresetToSlot(src_preset, dest_preset);
                select_source_pad = 0xFF;
                if (ok) {
                  select_mode_active = false;
                  copy_failed_pad = -1;
                  stateUpdated(CALL_MODE_BUTTON);
                } else {
                  // Internal failure (JSON busy, source missing,
                  // file write failed). Flash destination red while
                  // select key is still held; user can release and
                  // re-press to retry.
                  copy_failed_pad = (int8_t)d1;
                  stateUpdated(CALL_MODE_BUTTON);
                }
              }
            }
          }
          return;
        }

        if (shift_held) {
          // Shift + pad: SAVE by default; DELETE when delete_enabled is true
          // AND the preset slot is already occupied.
          //
          // Delete is a two-press confirmation. The first press arms
          // (paintPads flashes the pad red-fast-blink until shift is
          // released, the timeout elapses, or the same pad is pressed
          // again). Releasing shift cancels the arm — paintPads then
          // reverts the pad to its normal color. The second press
          // (shift still held) confirms and deletes.
          if (delete_enabled && getCachedPresetExists(preset)) {
            if (delete_armed_pad == d1) {
              // Second press while shift still held: confirm delete.
              delete_armed_pad = -1;
              deletePreset((uint8_t)preset);
              tracked_active_preset = 0;  // no preset active after delete (unless GUI restores)
              stateUpdated(CALL_MODE_BUTTON_PRESET);
              // The onEvent(PresetListMutated) handler forces the WS
              // push with cooldown bypass — no need to call
              // updateInterfaces() here.
            } else {
              // First press: arm. Paint will flash red until shift is
              // released (cancel), the timeout elapses (cancel), or the
              // same pad is pressed again (confirm).
              delete_armed_pad = (int8_t)d1;
              delete_armed_ms  = millis();
              stateUpdated(CALL_MODE_BUTTON);
            }
          } else {
            // Default: save current state to this preset slot. Also clears
            // any pending delete arm — the user is changing their mind to
            // a save action. The preset number in the name uses the MAPPED
            // slot (e.g., preset 1) not the raw MIDI note, so the label
            // matches what the user sees in the WLED UI.
            if (delete_armed_pad >= 0) {
              delete_armed_pad = -1;
            }
            char name[40];
            char suffix[16];
            snprintf(suffix, sizeof(suffix), " (Pad %d)", (int)preset);
            buildPresetSaveName(name, sizeof(name), suffix);
            savePreset((uint8_t)preset, name);
            // doSaveState() automatically updates presetCache[index].exists,
            // so onStateChange's getCachedPresetExists() check will now show
            // this slot as saved without any JSON buffer acquisition.
            // Stay on the currently-active preset — saving doesn't switch.
            stateUpdated(CALL_MODE_BUTTON_PRESET);
            // The onEvent(PresetListMutated) handler forces the WS
            // push with cooldown bypass — no need to call
            // updateInterfaces() here.
          }
        } else {
          // Apply preset using the canonical "switch to a preset cleanly"
          // pattern from usermod_v2_pioneer_prolink.h:1399-1402.
          if (strip.getSegmentsNum() > 1) strip.resetSegments(false);
          unloadPlaylist();  // best-effort; no-op if no playlist
          tracked_active_preset = (uint8_t)preset;  // survives currentPreset=0 wipes from stateUpdated
          applyPreset((uint8_t)preset, CALL_MODE_BUTTON_PRESET);
          handlePresets();
          // applyPreset already calls stateUpdated internally, which
          // fires our onStateChange and fills the OUT queue with the
          // correct pad paint (including the playlist child fast-blink
          // for a playlist preset). We just need to drain the queue
          // immediately rather than waiting ~1s for the main loop's
          // midi_usb_poll() to run — which is held up by the playlist
          // engine's JSON lock while it reads /presets.json.
          //
          // Earlier versions of this code also called stateUpdated
          // here to "force" a paint, but that races the natural one
          // from applyPreset and overwrites the correct child-blink
          // paint with a wrong "regular preset" paint. Forcing the
          // drain alone gives instant feedback without the race.
          midi_usb_poll();
        }
        return;
      }
      if (d1 >= kNoteFirstTrack && d1 < kNoteFirstTrack + kNumTracks) {     // Track buttons 100..107
        // Plain press runs track_button_action[N]; shift+press runs
        // track_button_shift_action[N] (which is allowed to be empty —
        // runAction() turns that into a no-op).
        const uint8_t ti = d1 - kNoteFirstTrack;
        const char* act = shift_held
                              ? track_button_shift_action[ti]
                              : track_button_action[ti];
        // Any non-reboot Track press also cancels an in-progress
        // reboot arm — accidental press shouldn't leave it stuck.
        // We skip the cancel for the reboot button itself; if the user
        // accidentally presses the same button again, runAction will
        // re-arm (first press) or execute (second press) per the
        // arm/execute state machine. Cancelling first would re-arm on
        // every press and the user could never actually execute.
        if (reboot_armed && strcmp(act, "rebootArm") != 0) {
          reboot_armed = false;
        }
        runAction(act);
        return;
      }
      if (d1 >= kNoteFirstScene && d1 < kNoteFirstScene + kNumScenes) {     // Scene launches 112..119
        // Plain press runs scene_button_action[N]; shift+press runs
        // scene_button_shift_action[N] (which is allowed to be empty —
        // runAction() turns that into a no-op).
        const uint8_t si = d1 - kNoteFirstScene;
        const char* act = shift_held
                              ? scene_button_shift_action[si]
                              : scene_button_action[si];
        // Any Scene press also cancels an in-progress reboot arm —
        // accidental press shouldn't leave it stuck.
        if (reboot_armed) reboot_armed = false;
        runAction(act);
        return;
      }
      return;
    }

    if (status == 0x80) {                // NoteOff
      if (d1 == kNoteShift) {
        shift_held = false;
        // Releasing shift cancels BOTH the reboot arm and the delete
        // arm (matches the velocity-0 NoteOn path above). Trigger a
        // repaint so the affected button/pad reverts to its normal
        // color instead of staying in the red-flash armed state.
        bool repaint = false;
        if (reboot_armed) {
          reboot_armed = false;
          repaint = true;
        }
        if (delete_armed_pad >= 0) {
          delete_armed_pad = -1;
          repaint = true;
        }
        if (repaint) stateUpdated(CALL_MODE_BUTTON);
      }
      // Pairs with the NoteOn "hold to activate" handling above.
      // Releases select mode and clears any pending copy-failure
      // flash so the destination pad reverts to its normal color.
      if (d1 == kNoteFirstScene + 6) {
        select_mode_active = false;
        if (copy_failed_pad >= 0) {
          copy_failed_pad = -1;
          stateUpdated(CALL_MODE_BUTTON);
        }
      }
      return;
    }

    if (status == 0xB0) {                // Control Change
      if (save_preset_cc != 0 && d1 == save_preset_cc && d2 >= 1 && d2 <= 16) {
        char name[40];
        char suffix[16];
        snprintf(suffix, sizeof(suffix), " (CC %d)", (int)save_preset_cc);
        buildPresetSaveName(name, sizeof(name), suffix);
        savePreset(d2, name);
        // doSaveState() already updated presetCache[index].exists; the next
        // onStateChange will see the new state via getCachedPresetExists().
        stateUpdated(CALL_MODE_BUTTON_PRESET);
        // The onEvent(PresetListMutated) handler forces the WS push
        // with cooldown bypass — no need to call updateInterfaces()
        // here.
        return;
      }
      // Soft takeover: if enabled and this CC hasn't been "taken over" yet
      // (no recent continuous movement near the current WLED value), drop
      // the packet unless d2 is within SOFT_TAKEOVER_WIGGLE of the CC
      // value that would map to the current parameter. The first time the
      // fader crosses the current value (with wiggle) we mark it taken
      // over and start driving the parameter.
      if (soft_takeover_enabled && d1 < kNumCCs && !cc_taken_over[d1]) {
        const char* act = cc_to_action[d1];
        if (act[0] != '\0') {
          int current_value = -1;
          if      (!strcmp(act, "bri"))            current_value = bri;
          else if (!strcmp(act, "effectSpeed"))    current_value = effectSpeed;
          else if (!strcmp(act, "effectIntensity"))current_value = effectIntensity;
          else if (!strcmp(act, "effectPalette")) current_value = effectPalette;
          else if (!strcmp(act, "effectCurrent")) current_value = effectCurrent;
          else if (!strcmp(act, "effectCustom1")) current_value = strip.getMainSegment().custom1;
          else if (!strcmp(act, "effectCustom2")) current_value = strip.getMainSegment().custom2;
          // For bri / speed / intensity / custom1 / custom2: WLED value is
          // CC*2, so expected CC = value/2. For palette / mode, the value
          // is bounded by strip count; expected CC = (value/2) clamped to
          // CC range. All these have a clear 1:1 correspondence to CC.
          int expected_cc = (current_value >= 0) ? (current_value >> 1) : -1;
          if (expected_cc >= 0) {
            int delta = (int)d2 - expected_cc;
            if (delta < 0) delta = -delta;
            if (delta > SOFT_TAKEOVER_WIGGLE) {
              USER_PRINTF("[MIDI] CC %u dropped (val=%u, expected~%u, wiggle=%u)\n",
                           (unsigned)d1, (unsigned)d2, (unsigned)expected_cc,
                           (unsigned)SOFT_TAKEOVER_WIGGLE);
              return;  // drop packet; fader hasn't reached the value yet
            }
            // Within wiggle — take over and apply below.
          }
          // unknown / un-mapped action: skip the takeover gate, apply.
        }
        cc_taken_over[d1] = 1;
      }
      runAction(cc_to_action[d1], d2);
      return;
    }
  }

  // Wipe every LED on the controller — all 64 RGB pads (NoteOn velocity
  // 0 = off) and all 16 single-color buttons (Track 1-8 + Scene 1-8).
  // Used right before ESP.restart() so the user sees the device go dark
  // rather than freeze mid-state.
  void wipeAllLeds() {
    // All 64 pads off
    for (uint8_t i = 0; i < kNumPads; i++) {
      midi_out_queue(0x96, i, 0);
    }
    // Track 1-8 (notes 100..107) off
    for (uint8_t n = kNoteFirstTrack; n <= kNoteLastTrack; n++) midi_out_queue(0x90, n, 0);
    // Scene 1-8 (notes 112..119) off
    for (uint8_t n = kNoteFirstScene; n <= kNoteLastScene; n++) midi_out_queue(0x90, n, 0);
  }

  // ---------------------------------------------------------------------------
  // Usermod v2 boilerplate (config persistence + JSON info)
  // ---------------------------------------------------------------------------

  void addToConfig(JsonObject& root) override {
    Usermod::addToConfig(root);
    JsonObject top = root[FPSTR(_name)];
    if (top.isNull()) top = root.createNestedObject(FPSTR(_name));

    // Top-level toggles. Pad mapping is fixed (top-left physical →
    // logical preset 1..64), Music Playlist Id is read live from the
    // AutoPlaylist usermod, and the CC map is hidden (every fader has
    // a hardcoded default action).
    top[FPSTR(_key_enabled)]               = enabled;
    top[FPSTR(_key_feedback_enabled)]      = feedback_enabled;
    top["copy_enabled"]                    = copy_enabled;
    top["delete_enabled"]                  = delete_enabled;
    top["reboot_enabled"]                  = reboot_enabled;
    top["soft_takeover_enabled"]           = soft_takeover_enabled;
    top["target_segment"]                  = target_segment;

    // Track buttons 1..8, each with a plain and shift+ variant.
    // Two parallel objects: track_buttons (plain) and track_buttons_shift.
    // The GUI renders these as 16 dropdowns (8 plain + 8 shift).
    JsonObject trkObj = top.createNestedObject("track_buttons");
    JsonObject trkShiftObj = top.createNestedObject("track_buttons_shift");
    for (int i = 0; i < 8; i++) {
      char key[4]; snprintf(key, sizeof(key), "%d", i + 1);
      trkObj[key] = track_button_action[i];
      trkShiftObj[key] = track_button_shift_action[i];
    }

    // Scene launches 1..8, plain + shift variants. Shift is omitted for
    // scenes 6 and 7 — assigning a shift action to those puts the APC
    // into internal "note" / "drum" modes, so the GUI doesn't expose
    // shift for them.
    JsonObject scnObj = top.createNestedObject("scene_buttons");
    JsonObject scnShiftObj = top.createNestedObject("scene_buttons_shift");
    for (int i = 0; i < 8; i++) {
      char key[4]; snprintf(key, sizeof(key), "%d", i + 1);
      scnObj[key] = scene_button_action[i];
      if (i < 6) scnShiftObj[key] = scene_button_shift_action[i];
    }
  }

  bool readFromConfig(JsonObject& root) override {
    bool configComplete = Usermod::readFromConfig(root);
    JsonObject top = root[FPSTR(_name)];
    if (top.isNull()) return configComplete;

    configComplete &= getJsonValue(top[FPSTR(_key_enabled)],              enabled, true);
    configComplete &= getJsonValue(top[FPSTR(_key_feedback_enabled)],     feedback_enabled, true);
    configComplete &= getJsonValue(top["copy_enabled"],                   copy_enabled, true);
    configComplete &= getJsonValue(top["delete_enabled"],                 delete_enabled, true);
    configComplete &= getJsonValue(top["reboot_enabled"],                 reboot_enabled, true);
    configComplete &= getJsonValue(top["soft_takeover_enabled"],          soft_takeover_enabled, true);
    configComplete &= getJsonValue(top["target_segment"],                 target_segment, (uint8_t)0);

    // Track buttons (plain): "1".."8" → track_button_action[0..7].
    // Missing keys keep the defaults set by the constructor.
    JsonObject trkObj = top["track_buttons"];
    if (!trkObj.isNull()) {
      for (int i = 1; i <= 8; i++) {
        char key[4]; snprintf(key, sizeof(key), "%d", i);
        const char* act = trkObj[key].as<const char*>();
        if (act) setAction(track_button_action[i - 1], act);
      }
    }

    // Track buttons (shift+): "1".."8" → track_button_shift_action[0..7].
    JsonObject trkShiftObj = top["track_buttons_shift"];
    if (!trkShiftObj.isNull()) {
      for (int i = 1; i <= 8; i++) {
        char key[4]; snprintf(key, sizeof(key), "%d", i);
        const char* act = trkShiftObj[key].as<const char*>();
        if (act) setAction(track_button_shift_action[i - 1], act);
      }
    }

    // Scene launches (plain): "1".."8" → scene_button_action[0..7].
    JsonObject scnObj = top["scene_buttons"];
    if (!scnObj.isNull()) {
      for (int i = 1; i <= 8; i++) {
        char key[4]; snprintf(key, sizeof(key), "%d", i);
        const char* act = scnObj[key].as<const char*>();
        if (act) setAction(scene_button_action[i - 1], act);
      }
    }

    // Scene launches (shift+): "1".."6" → scene_button_shift_action[0..5].
    // Scenes 6 and 7 (idx 5, 6) intentionally have no shift binding.
    JsonObject scnShiftObj = top["scene_buttons_shift"];
    if (!scnShiftObj.isNull()) {
      for (int i = 1; i <= 6; i++) {
        char key[4]; snprintf(key, sizeof(key), "%d", i);
        const char* act = scnShiftObj[key].as<const char*>();
        if (act) setAction(scene_button_shift_action[i - 1], act);
      }
    }

    return configComplete;
  }

  void addToJsonInfo(JsonObject& root) override {
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");
    JsonArray infoArr = user.createNestedArray(FPSTR(_name));
    if (midi_connected) {
      char buf[64];
      if (midi_device_name[0]) {
        snprintf(buf, sizeof(buf), "%s (0x%04X:0x%04X)",
                 midi_device_name, midi_vendor, midi_product);
      } else {
        snprintf(buf, sizeof(buf), "0x%04X:0x%04X", midi_vendor, midi_product);
      }
      infoArr.add(buf);
    } else {
      infoArr.add("disconnected");
    }
  }

  // WLED-MM calls appendConfigData() once per usermod when the
  // settings page for that usermod is loaded. We use it to upgrade
  // the auto-generated text inputs for our track/scene action
  // assignments into dropdowns with the full verb list (and the
  // current value pre-selected).
  //
  // For each "1".."8" key under track_buttons / track_buttons_shift /
  // scene_buttons / scene_buttons_shift, we find the corresponding
  // text input in the rendered form (which WLED-MM generates from
  // addToConfig's nested-object string fields) and replace it with
  // a <select> listing every valid action verb.
  void appendConfigData() override {
    // Helper: emit JS that converts the text input for one key
    // (under our usermod's namespace) into a dropdown.
    //
    // The JS template's addDropdown(usermod, fieldPath) builds the
    // selector as "<usermod>:<fieldPath>", so the second arg must be
    // ONLY the path relative to the usermod root (no usermod prefix).
    auto emit_dropdown = [&](const char* group, uint8_t idx, bool shift) {
      char path[24];
      snprintf(path, sizeof(path), "%s:%d", group, (int)(idx + 1));
      oappend(SET_F("var __s = addDropdown('MIDI-USB','"));
      oappend(path);
      oappend(SET_F("');"));
      // Empty option so the user can clear an assignment.
      oappend(SET_F("addOption(__s,'(unassigned)','');"));
      // All available action verbs, with the ones that are defaults
      // for this button+shift combination marked "(default)".
      // The verb list is the same kGUIVerbs that runAction() dispatches.
      for (uint8_t v = 0; v < kNumGUIVerbs; v++) {
        // Mark defaults so the user can see what's pre-assigned.
        const char* suffix = "";
        bool is_default = false;
        const bool is_track = (group && !strcmp(group, "track_buttons"));
        const bool is_scene = (group && !strcmp(group, "scene_buttons"));
        if (!shift && is_track) {
          // Plain defaults for TRACK buttons only.
          if (idx == 0 && !strcmp(kGUIVerbs[v], "toggleCheck1")) is_default = true;
          if (idx == 1 && !strcmp(kGUIVerbs[v], "toggleCheck2")) is_default = true;
          if (idx == 2 && !strcmp(kGUIVerbs[v], "toggleCheck3")) is_default = true;
          if (idx == 3 && !strcmp(kGUIVerbs[v], "fullRepaint")) is_default = true;
          if (idx == 4 && !strcmp(kGUIVerbs[v], "prevfx")) is_default = true;
          if (idx == 5 && !strcmp(kGUIVerbs[v], "nextfx")) is_default = true;
          if (idx == 6 && !strcmp(kGUIVerbs[v], "prevpreset")) is_default = true;
          if (idx == 7 && !strcmp(kGUIVerbs[v], "nextpreset")) is_default = true;
        } else if (shift && is_track) {
          // Shift defaults for TRACK buttons only.
          if (idx == 0 && !strcmp(kGUIVerbs[v], "toggleCheck1")) is_default = true;
          if (idx == 1 && !strcmp(kGUIVerbs[v], "toggleCheck2")) is_default = true;
          if (idx == 2 && !strcmp(kGUIVerbs[v], "toggleCheck3")) is_default = true;
          if (idx == 3 && !strcmp(kGUIVerbs[v], "fullRepaint")) is_default = true;
          if (idx == 4 && !strcmp(kGUIVerbs[v], "prevpal")) is_default = true;
          if (idx == 5 && !strcmp(kGUIVerbs[v], "nextpal")) is_default = true;
          if (idx == 6 && !strcmp(kGUIVerbs[v], "prevpreset")) is_default = true;
          if (idx == 7 && !strcmp(kGUIVerbs[v], "nextpreset")) is_default = true;
        } else if (!shift && is_scene) {
          // Scene defaults for SCENE buttons only.
          if (idx == 0 && !strcmp(kGUIVerbs[v], "toggleMirrorX")) is_default = true;
          if (idx == 1 && !strcmp(kGUIVerbs[v], "toggleReverseX")) is_default = true;
          if (idx == 2 && !strcmp(kGUIVerbs[v], "toggleMirrorY")) is_default = true;
          if (idx == 3 && !strcmp(kGUIVerbs[v], "toggleReverseY")) is_default = true;
          if (idx == 4 && !strcmp(kGUIVerbs[v], "toggleTranspose")) is_default = true;
          if (idx == 6 && !strcmp(kGUIVerbs[v], "selectMode")) is_default = true;
          if (idx == 7 && !strcmp(kGUIVerbs[v], "power")) is_default = true;
        }
        // (scene_buttons_shift has no defaults — leave unassigned)
        if (is_default) suffix = " (default)";
        oappend(SET_F("addOption(__s,'"));
        oappend(kGUIVerbs[v]);
        oappend(suffix);
        oappend(SET_F("','"));
        oappend(kGUIVerbs[v]);
        oappend(SET_F("');"));
      }
    };

    // Dropdowns: 8 track plain + 8 track shift + 8 scene plain +
    // 6 scene shift (scenes 6 and 7 are intentionally excluded —
    // assigning shift actions to them puts the APC into internal
    // "note" / "drum" modes we don't want).
    const char* groups[] = {
      "track_buttons", "track_buttons_shift",
      "scene_buttons", "scene_buttons_shift",
    };
    for (uint8_t g = 0; g < 4; g++) {
      bool shift = (g == 1 || g == 3);
      const bool is_scene = (g >= 2);
      const uint8_t max_i = (shift && is_scene) ? 6 : 8;
      for (uint8_t i = 0; i < max_i; i++) {
        emit_dropdown(groups[g], i, shift);
      }
    }
  }

  uint16_t getId() override { return USERMOD_ID_MIDI_USB; }
};
// FPSTR string constants are defined inline above as static constexpr —
// PROGMEM is a no-op on ESP32, so there's no need for out-of-class
// definitions (which would otherwise cause ODR violations when this header
// is included from multiple TUs).
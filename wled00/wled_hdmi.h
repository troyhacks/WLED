#pragma once
// HDMI display subsystem — Olimex ESP32-P4-PC via LT8912B bridge.
// Public interface called from wled.cpp; internals are in wled_hdmi.cpp.

#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(WLEDMM_DISPLAY_MODE)

// ── Parsed EDID data from the connected monitor. ─────────────────────────────
// Populated by hdmi_edid_init() (called inside hdmi_setup() after link lock).
// All fields are zero/empty if no monitor is connected or EDID read fails.
struct hdmi_edid_info_t {
  bool     present;            // true = valid EDID was read
  char     manufacturer[4];   // 3-letter JEDEC code e.g. "SAM"
  uint16_t product_code;
  char     monitor_name[14];  // null-terminated name from 0xFC descriptor
  uint16_t year;              // year of manufacture

  // Preferred timing (first non-zero Detailed Timing Descriptor)
  uint32_t preferred_pclk_khz;
  uint16_t preferred_hactive;
  uint16_t preferred_vactive;

  // Standard timings (bytes 38-53, up to 8 entries)
  uint8_t  std_timing_count;
  uint16_t std_hactive[8];
  uint16_t std_vactive[8];

  // CEA-861 extension data (if extension block is present)
  bool     hdmi_vsdb_found;    // HDMI LLC VSDB found → monitor speaks HDMI (not just DVI)
  uint8_t  cea_vic_count;
  uint8_t  cea_vic[32];        // VIC codes advertised in CEA Video Data Block
};
extern hdmi_edid_info_t hdmi_edid_info;

void hdmi_setup();               // boot: init bridge and display
void hdmi_print_mode_menu();     // print numbered mode list to Serial
void hdmi_switch_mode(int mode); // runtime mode switch
#if defined(CONFIG_SOC_PPA_SUPPORTED)
void hdmi_blit();                // loop: PPA scale-blit LED bus buffer → HDMI framebuffer
#endif
#endif // CONFIG_IDF_TARGET_ESP32P4 && WLEDMM_DISPLAY_MODE

#pragma once
// The HDMI bridge backend.
//
// The LT8912B takes the P4's MIPI-DSI and gives HDMI out to a monitor.
// The display layer (wled_display.cpp) owns the picture; this owns the
// bridge: its modes, the monitor's EDID, and its registers.

#if defined(CONFIG_IDF_TARGET_ESP32P4)

// ── Parsed EDID data from the connected monitor. ─────────────────────────────
// Populated by hdmi_edid_init() (called inside the bridge's init, after the
// link is up).  All fields are zero/empty if no monitor is connected or the
// EDID read fails.
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

// Is there an LT8912B on the I2C bus at all?
bool hdmiBridgePresent();

// Is a monitor plugged into the bridge?
bool hdmiBridgeLinkPresent();

// The modes the bridge can drive.
int         hdmiBridgeModeCount();
const char* hdmiBridgeModeName(int mode);
int         hdmiBridgeModeIndex(const char* name);   // -1 when there is no such mode
int         hdmiBridgeCurrentMode();

// The mode that is up: its size.
uint16_t hdmiBridgeWidth();
uint16_t hdmiBridgeHeight();

// Bring the bridge up on a mode, and take it down again.
bool hdmiBridgeInit(int mode);
void hdmiBridgeDeinit();

// The Art-Net output follows the link: this polls it.
void hdmiBridgePollLink();

// The console's mode menu.
void hdmi_print_mode_menu();
void hdmi_switch_mode(int mode);

#endif // CONFIG_IDF_TARGET_ESP32P4

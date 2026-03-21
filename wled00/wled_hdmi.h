#pragma once
// HDMI display subsystem — Olimex ESP32-P4-PC via LT8912B bridge.
// Public interface called from wled.cpp; internals are in wled_hdmi.cpp.

#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(WLEDMM_DISPLAY_MODE)
void hdmi_setup();               // boot: print mode menu, wait for Serial input, call hdmi_display_init()
void hdmi_print_mode_menu();     // print numbered mode list (called from handleSerial before waiting for input)
void hdmi_switch_mode(int mode); // runtime: switch to mode index (called from handleSerial after reading number)
#if defined(CONFIG_SOC_PPA_SUPPORTED)
void hdmi_blit();           // loop: PPA scale-blit LED bus buffer → HDMI framebuffer
#endif
#endif // CONFIG_IDF_TARGET_ESP32P4 && WLEDMM_DISPLAY_MODE

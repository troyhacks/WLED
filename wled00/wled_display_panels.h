#pragma once
// The table of panels the P4 can drive.
//
// The board does not know what is on its MIPI-DSI connector at build time,
// so every supported panel is described here: its geometry, its timing, its
// touch controller, and the two pins that may or may not be wired up.
//
// The table is in flash and is never written to, so it costs no RAM.
//
// The timings are the ones the vendors ship with their own drivers:
//   ILI9881C - Espressif's ILI9881C_800_1280_PANEL_60HZ_DPI_CONFIG
//   EK79007  - Espressif's EK79007_1024_600_PANEL_60HZ_CONFIG
//   ST7703   - Waveshare's ST7703_720_720_PANEL_60HZ_DPI_CONFIG
//   JD9365  - the Waveshare 10.1" panel, as it was brought up on the
//              HDMI-Experiment branch (40 MHz, 480 Mbps per lane)

#if defined(CONFIG_IDF_TARGET_ESP32P4)

// The panel controller.  Which one it is decides which driver brings it up.
enum display_driver_t : uint8_t {
  DRV_ILI9881C = 0,
  DRV_EK79007,
  DRV_ST7703,
  DRV_JD9365,
  DRV_COUNT
};

// The touch controller on the panel.
enum display_touch_t : uint8_t {
  TCH_NONE = 0,
  TCH_GT911
};

// What is in the panel's framebuffer.
#define DSI_FLAG_TOUCH        0x01  // it has a touch controller
#define DSI_FLAG_ROTATE       0x02  // the picture can be turned
#define DSI_FLAG_BACKLIGHT    0x04  // the backlight is on a pin of ours
#define DSI_FLAG_BACKLIGHT_LO 0x08  // ... and it is on when the pin is LOW

struct dsi_panel_t {
  const char *name;            // what the settings page calls it
  uint8_t     driver;          // display_driver_t
  uint16_t    width, height;   // the panel's own pixels
  uint8_t     lanes;           // the DSI data lanes
  uint32_t    lane_mbps;       // the bit rate of one lane
  uint32_t    dpi_mhz;         // the pixel clock
  uint16_t    hbp, hsw, hfp;   // the horizontal timing
  uint16_t    vbp, vsw, vfp;   // the vertical timing
  uint8_t     rgb_swap;        // the PPA swaps the red and the blue
  uint8_t     ele_bgr;         // the panel is told BGR, not RGB
  int8_t      reset_gpio;      // the panel's reset line, -1 = none
  int8_t      backlight_gpio;  // the panel's backlight, -1 = none
  uint8_t     touch;           // display_touch_t
  uint8_t     touch_addr;      // the touch controller's I2C address
  uint16_t    touch_x_max, touch_y_max;
  uint8_t     flags;           // DSI_FLAG_*
};

// The panels.
//
// The reset and backlight pins are the board's business, not the panel's:
// they are different on every carrier, so they are settings and the table
// only carries a default.  The defaults here are the ones the panels were
// brought up with.
static const dsi_panel_t dsi_panels[DRV_COUNT] = {
  // name                 driver          w     h    lanes lane  dpi  hbp hsw hfp  vbp vsw vfp  swap bgr rst  bl   touch          addr  xmax  ymax  flags
  { "ILI9881C 800x1280", DRV_ILI9881C,  800, 1280, 2, 1000, 80, 140, 40, 40,  16,  4, 16,  0,  1,   -1,  -1, TCH_GT911, 0x5D,  800, 1280, DSI_FLAG_TOUCH | DSI_FLAG_ROTATE },
  { "EK79007 1024x600",   DRV_EK79007,   1024,  600, 2,  900, 52, 160, 10, 160, 23,  1, 12,  0,  1,   -1,  -1, TCH_GT911, 0x5D, 1024,  600, DSI_FLAG_TOUCH | DSI_FLAG_ROTATE },
  // The Smart 86 Box's panel: the reset is on GPIO 27 and the backlight on
  // GPIO 26, and the backlight is ON when the pin is LOW.
  { "ST7703 720x720",     DRV_ST7703,     720,  720, 2,  720, 60, 120, 60, 106, 20,  4, 20,  0,  1,   27,  26, TCH_GT911, 0x5D,  720,  720, DSI_FLAG_TOUCH | DSI_FLAG_ROTATE | DSI_FLAG_BACKLIGHT | DSI_FLAG_BACKLIGHT_LO },
  { "JD9365 800x1280",    DRV_JD9365,     800, 1280, 2,  480, 40,  20, 20, 40,  10,  4, 30,  0,  1,   -1,  -1, TCH_GT911, 0x5D,  800, 1280, DSI_FLAG_TOUCH | DSI_FLAG_ROTATE },
};

// The panel's name, or NULL when there is no such panel.
inline const dsi_panel_t* dsiPanelByName(const char* name) {
  if (!name || !name[0]) return nullptr;
  for (uint8_t i = 0; i < DRV_COUNT; i++)
    if (strncmp(dsi_panels[i].name, name, 24) == 0) return &dsi_panels[i];
  return nullptr;
}

// How many panels there are, and what they are called.
inline uint8_t dsiPanelCount(void) { return DRV_COUNT; }
inline const char* dsiPanelName(uint8_t i) {
  return (i < DRV_COUNT) ? dsi_panels[i].name : "";
}

#endif // CONFIG_IDF_TARGET_ESP32P4

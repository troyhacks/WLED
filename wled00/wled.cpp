#define WLED_DEFINE_GLOBAL_VARS //only in one source file, wled.cpp!
static const char *TAG = "WLED";
#include "wled.h"
#include "wled_ethernet.h"
#include <Arduino.h>
#if !defined(CONFIG_SLAVE_SOC_WIFI_HE_SUPPORT)
  #define CONFIG_SLAVE_SOC_WIFI_HE_SUPPORT 0
#endif
#if defined(CONFIG_IDF_TARGET_ESP32P4)
  #include "esp_ldo_regulator.h"  // ESP32-P4 LDO control
  #include "driver/i2c_master.h"  // IDF v5 I2C master API
  #include "esp_lcd_panel_ops.h"
  #include "esp_lcd_panel_io.h"
  #include "esp_lcd_mipi_dsi.h"
  #include "esp_lcd_lt8912b.h"    // Olimex ESP32-P4-PC HDMI bridge (LT8912B)
  #include <LovyanGFX.hpp>
  static LGFX_Sprite myFramebuffer;
  static LGFX_Sprite buttonFramebuffer;
  // ESP32-P4 Board Log
  // ESP-ROM:esp32p4-eco2-20240710 // Olimex ESP32-P4-PC (HDMI output via LT8912B)
  // ESP-ROM:esp32p4-eco2-20240710 // WaveShare Nano
  // ESP-ROM:esp32p4-eco2-20240710 // WaveShare P4 Module Dev Kit (4 USB-A ports, custom module)
  // ESP-ROM:esp32p4-eco2-20240710 // WaveShare Big Round Display Thingy
  // ESP-ROM:esp32p4-eco2-20240710 // WaveShare ESP32-P4-86-Panel-ETH-PRO
  // ESP-ROM:esp32p4-eco1-20240205 // Espressif EV
  // ESP-ROM:esp32p4-eco2-20240710 // Wireless Tag Fancy C5 board that's weird.
  // ESP-ROM:esp32p4-eco2-20240710 // WaveShare ESP32-P4-WIFI6-POE-ETH

#endif // CONFIG_IDF_TARGET_ESP32P4

// === CEA-861 / LT8912B HDMI video timing — file scope so WLEDMM_DISPLAY_MODE can be used as a compile flag ===
#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(WLEDMM_DISPLAY_MODE)

// Aspect ratio values for HDMI AVI InfoFrame PB2 (matches LT8912B driver constants)
#define HDMI_AR_NONE  0x00  // No data / no standard mapping (e.g. 16:10)
#define HDMI_AR_4_3   0x01
#define HDMI_AR_16_9  0x02

// X-macro table — single source of truth for all HDMI modes.
// To add/remove a mode: edit only this list.
// Columns: name, vic, fps, width, height, pclk_khz, hfp, hsw, hbp, vfp, vsw, vbp, aspect_ratio
//          Verified: (width+hfp+hsw+hbp) * (height+vfp+vsw+vbp) * fps = pclk_khz*1000
#define HDMI_MODE_LIST \
  /* --- CEA-861 standard modes (vic > 0) --- */ \
  /* 480p: officially supported by LT8912B (datasheet §5.1). 59.94Hz = 27027kHz, 60.00Hz = 27000kHz */ \
  /* NOTE: vic>0 (standard CEA blanking) modes consistently show no display — ESP-IDF driver  */ \
  /* appears only validated for vic=0 (LT8912B reduced blanking) reference modes.             */ \
  X(CEA_480P_60HZ_NO_DISPLAY,               2, 60,  720,  480,  27027,   16, 62,  60,  9,  6, 30, HDMI_AR_4_3 ) /* Htot=858  Vtot=525  VIC 2 4:3  */ \
  X(CEA_480P_60HZ_WIDE_NO_DISPLAY,          3, 60,  720,  480,  27027,   16, 62,  60,  9,  6, 30, HDMI_AR_16_9) /* Htot=858  Vtot=525  VIC 3 16:9 */ \
  X(CEA_VGA_480P_60HZ_GARBLED,           1, 60,  640,  480,  25175,   16, 96,  48, 10,  2, 33, HDMI_AR_4_3 ) /* Htot=800  Vtot=525  VIC 1 640x480 */ \
  /* vic=0 reduced-blanking 480p — test if driver works with non-CEA blanking at this res   */ \
  X(LT8912B_480P_60HZ_NO_DISPLAY,           0, 60,  720,  480,  27000,   48, 32,  80,  3,  5, 13, HDMI_AR_4_3 ) /* Htot=880  Vtot=501  no display — monitor may not accept 480p, or ESP-IDF driver untested at this res */ \
  X(HDMI_720P_24HZ_NO_DISPLAY,             60, 24, 1280,  720,  59400, 1760, 40, 220,  5,  5, 20, HDMI_AR_16_9) /* Htot=3300 Vtot=750  */ \
  X(HDMI_720P_25HZ_NO_DISPLAY,             61, 25, 1280,  720,  74250, 2420, 40, 220,  5,  5, 20, HDMI_AR_16_9) /* Htot=3960 Vtot=750  */ \
  X(HDMI_720P_30HZ_NO_DISPLAY,             62, 30, 1280,  720,  74250, 1760, 40, 220,  5,  5, 20, HDMI_AR_16_9) /* Htot=3300 Vtot=750  */ \
  X(HDMI_720P_50HZ_NO_DISPLAY,             19, 50, 1280,  720,  74250,  440, 40, 220,  5,  5, 20, HDMI_AR_16_9) /* Htot=1980 Vtot=750  */ \
  X(HDMI_720P_60HZ_UNDERRUN_AND_GARBLE,              4, 60, 1280,  720,  74250,  110, 40, 220,  5,  5, 20, HDMI_AR_16_9) /* Htot=1650 Vtot=750  */ \
  X(HDMI_1080P_24HZ_NO_DISPLAY,            32, 24, 1920, 1080,  74250,  638, 44, 148,  4,  5, 36, HDMI_AR_16_9) /* Htot=2750 Vtot=1125 */ \
  X(HDMI_1080P_25HZ_NO_DISPLAY,            33, 25, 1920, 1080,  74250,  528, 44, 148,  4,  5, 36, HDMI_AR_16_9) /* Htot=2640 Vtot=1125 */ \
  X(HDMI_1080P_30HZ_UNDERRUN,            34, 30, 1920, 1080,  74250,   88, 44, 148,  4,  5, 36, HDMI_AR_16_9) /* Htot=2200 Vtot=1125 */ \
  X(HDMI_1080P_50HZ_CRASH,            31, 50, 1920, 1080, 148500,  528, 44, 148,  4,  5, 36, HDMI_AR_16_9) /* Htot=2640 Vtot=1125 */ \
  X(HDMI_1080P_60HZ_CRASH,            16, 60, 1920, 1080, 148500,   88, 44, 148,  4,  5, 36, HDMI_AR_16_9) /* Htot=2200 Vtot=1125 */ \
  /* --- VESA DMT standard modes (vic=0) --- */ \
  X(VESA_800x600_60HZ_OK,           0, 60,  800,  600,  40000,   40,128,  88,  1,  4, 23, HDMI_AR_4_3 ) /* Htot=1056 Vtot=628  */ \
  X(VESA_800x600_60HZ_WIDE,           0, 60,  800,  600,  40000,   40,128,  88,  1,  4, 23, HDMI_AR_16_9 ) /* Htot=1056 Vtot=628  */ \
  X(VESA_1024x768_60HZ_NO_DISPLAY,          0, 60, 1024,  768,  65000,   24,136, 160,  3,  6, 29, HDMI_AR_4_3 ) /* Htot=1344 Vtot=806  */ \
  /* --- LT8912B reference timings (vic=0, non-standard reduced blanking) --- */ \
  X(LT8912B_800x600_60HZ_OKISH,        0, 60,  800,  600,  40000,   48,128,  88,  1,  4, 23, HDMI_AR_4_3 ) /* Htot=1064 Vtot=628  */ \
  X(LT8912B_1024x768_60HZ_NO_DISPLAY,       0, 60, 1024,  768,  56000,   48, 32,  80,  3,  4, 15, HDMI_AR_4_3 ) /* Htot=1184 Vtot=790  */ \
  X(LT8912B_720P_60HZ_UNDERRUN,           0, 60, 1280,  720,  64000,   48, 32,  80,  3,  5, 13, HDMI_AR_16_9) /* Htot=1440 Vtot=741  */ \
  X(LT8912B_720P_56HZ_TROYHACKS, 0, 60, 1280,  720,  60000,   48, 32,  80,  3,  5, 13, HDMI_AR_16_9) /* Htot=1440 Vtot=741 experimental */ \
  X(LT8912B_1280x800_60HZ_UNDERRUN,       0, 60, 1280,  800,  70000,   48, 32,  80,  3,  6, 14, HDMI_AR_NONE) /* Htot=1440 Vtot=823 16:10 */ \
  X(LT8912B_1080P_30HZ_UNDERRUN,          0, 30, 1920, 1080,  70000,   48, 32,  80,  3,  5,  8, HDMI_AR_16_9) /* Htot=2080 Vtot=1096 */ \
  X(LT8912B_1080P_60HZ_UNDERRUN,          0, 60, 1920, 1080, 120000,   48, 32,  80,  3,  5, 19, HDMI_AR_16_9) /* Htot=2080 Vtot=1107 NOT WORKING per Espressif */ \
  /* --- Inno-Maker supported resolutions (VESA DMT, vic=0 unless CEA) --- */ \
  X(INNO_640x480_60HZ_SKEWED,              1, 60,  640,  480,  25175,   16, 96,  48, 10,  2, 33, HDMI_AR_4_3 ) /* Htot=800  Vtot=525  CEA VIC 1 */ \
  X(INNO_1152x864_60HZ_UNDERRUN,             0, 60, 1152,  864,  81624,   64,120, 184,  1,  3, 27, HDMI_AR_4_3 ) /* Htot=1520 Vtot=895  CVT */ \
  X(INNO_720x480_60HZ_NO_DISPLAY,              3, 60,  720,  480,  27000,   16, 62,  60,  9,  6, 30, HDMI_AR_16_9) /* Htot=858  Vtot=525  CEA VIC 3 (480p widescreen) */ \
  // X(INNO_1280x1024_60HZ,            0, 60, 1280, 1024, 108000,   48,112, 248,  1,  3, 38, HDMI_AR_NONE) /* Htot=1688 Vtot=1066 5:4 */ \
  // X(INNO_1440x900_60HZ,             0, 60, 1440,  900, 106500,   80,152, 232,  3,  6, 25, HDMI_AR_NONE) /* Htot=1904 Vtot=934  16:10 */ \
  // X(INNO_1600x900_60HZ,             0, 60, 1600,  900, 108000,   24, 80,  96,  1,  3, 96, HDMI_AR_16_9) /* Htot=1800 Vtot=1000 CVT-RB */ \
  // X(INNO_1680x1050_60HZ,            0, 60, 1680, 1050, 146250,  104,176, 280,  3,  6, 30, HDMI_AR_NONE) /* Htot=2240 Vtot=1089 16:10 */ \
  // X(INNO_1920x1080_60HZ,           16, 60, 1920, 1080, 148500,   88, 44, 148,  4,  5, 36, HDMI_AR_16_9) /* Htot=2200 Vtot=1125 CEA VIC 16 */

enum hdmi_mode_t : uint8_t {
#define X(name, ...) name,
  HDMI_MODE_LIST
#undef X
  HDMI_MODE_COUNT
};

struct hdmi_cea861_entry_t {
  uint8_t  vic;
  uint8_t  fps;
  uint16_t width;
  uint16_t height;
  uint32_t pixel_clock_khz;
  uint16_t hfp;  // hsync_front_porch
  uint16_t hsw;  // hsync_pulse_width
  uint16_t hbp;  // hsync_back_porch
  uint8_t  vfp;  // vsync_front_porch
  uint8_t  vsw;  // vsync_pulse_width
  uint8_t  vbp;  // vsync_back_porch
  uint8_t  aspect_ratio;  // HDMI_AR_NONE/4_3/16_9
};

static const char* const hdmi_mode_names[HDMI_MODE_COUNT] = {
#define X(name, ...) #name,
  HDMI_MODE_LIST
#undef X
};

static const hdmi_cea861_entry_t hdmi_cea861_table[HDMI_MODE_COUNT] = {
#define X(name, vic, fps, w, h, pclk, hfp, hsw, hbp, vfp, vsw, vbp, ar) \
  { vic, fps, w, h, pclk, hfp, hsw, hbp, vfp, vsw, vbp, ar },
  HDMI_MODE_LIST
#undef X
};

struct hdmi_dpi_config_t {
  uint8_t  vic;
  uint8_t  fps;
  uint16_t width;
  uint16_t height;
  uint32_t dpi_clock_freq_mhz;  // rounded to nearest MHz for ESP-IDF
  uint16_t hsync_front_porch;
  uint16_t hsync_pulse_width;
  uint16_t hsync_back_porch;
  uint8_t  vsync_front_porch;
  uint8_t  vsync_pulse_width;
  uint8_t  vsync_back_porch;
  uint16_t hsync_total;         // width + hfp + hsw + hbp
  uint16_t vsync_total;         // height + vfp + vsw + vbp
  uint32_t lane_bit_rate_mbps;  // 2x minimum for DSI lane overhead, rounded to next 50 Mbps
  uint8_t  aspect_ratio;        // HDMI_AR_NONE/4_3/16_9
};

static bool hdmi_get_dpi_config(hdmi_mode_t mode, uint8_t dsi_lanes, hdmi_dpi_config_t &out) {
  if (mode >= HDMI_MODE_COUNT) return false;
  const hdmi_cea861_entry_t &t = hdmi_cea861_table[mode];
  out.vic                = t.vic;
  out.fps                = t.fps;
  out.width           = t.width;
  out.height           = t.height;
  out.dpi_clock_freq_mhz = (t.pixel_clock_khz + 500) / 1000;  // round to nearest MHz
  out.hsync_front_porch  = t.hfp;
  out.hsync_pulse_width  = t.hsw;
  out.hsync_back_porch   = t.hbp;
  out.vsync_front_porch  = t.vfp;
  out.vsync_pulse_width  = t.vsw;
  out.vsync_back_porch   = t.vbp;
  out.hsync_total        = t.width + t.hfp + t.hsw + t.hbp;
  out.vsync_total        = t.height + t.vfp + t.vsw + t.vbp;
  // lane_bit_rate: minimum = pixel_clock × bpp / num_lanes
  // Empirically the ESP32-P4 DSI PHY stops working below ~480 Mbps (480p/27MHz fails at 324 Mbps,
  // 800x600/40MHz works at 480 Mbps). Clamp to 500 Mbps so low-res modes still operate.
  // The DPI pixel clock is independent — running the DSI lane faster just adds idle time in blanking.
  out.lane_bit_rate_mbps = (uint32_t)((uint64_t)t.pixel_clock_khz * 24 / dsi_lanes / 1000);
  if (out.lane_bit_rate_mbps < 500) out.lane_bit_rate_mbps = 500;
  out.aspect_ratio       = t.aspect_ratio;
  return true;
}

// Direct register read from an LT8912B I2C bus (stored io handle, 8-bit reg → 8-bit value).
// Returns 0xFF on error. Only valid after lt8912b_io_main is assigned in setup().
// Key regs (from Linux DRM driver lt8912.c):
//   MAIN 0xC1 bit 7 : HPD — cable plugged in. NOTE: HPD_CBUS pin defaults to MHL CBUS
//                     mode per datasheet §3.2.2; must be switched to HPD via I2C write.
//                     Validity depends on the ESP-IDF driver having done that config.
//   MAIN 0xB2 bit 0 : 1=HDMI mode, 0=DVI. Set from monitor EDID — but LT8912B has no
//                     DDC support (datasheet §2.2), so this likely always reads 0 (DVI).
static uint8_t lt8912b_read_reg(esp_lcd_panel_io_handle_t io, uint8_t reg) {
  uint8_t val = 0xFF;
  esp_lcd_panel_io_rx_param(io, reg, &val, 1);
  return val;
}

// Tear down the HDMI display stack (panel → I2C handles → DSI bus → PPA framebuffer → sprites).
// Sets display_framebuffer=NULL first so the blit loop stops immediately.
static void hdmi_display_deinit() {
  display_framebuffer = NULL;           // blit loop checks this — stops within one frame
  vTaskDelay(pdMS_TO_TICKS(100));       // let any in-progress PPA blit finish

  myFramebuffer.deleteSprite();
  buttonFramebuffer.deleteSprite();

  if (ppa_framebuffer) { heap_caps_free(ppa_framebuffer); ppa_framebuffer = NULL; }

  // Delete panel first (it may use I2C handles during teardown)
  if (panel_handle)    { esp_lcd_panel_del(panel_handle);         panel_handle    = NULL; }

  // Delete I2C handles after panel is gone
  if (lt8912b_io_avi)  { esp_lcd_del_panel_io(lt8912b_io_avi);   lt8912b_io_avi  = NULL; }
  if (lt8912b_io_cec)  { esp_lcd_del_panel_io(lt8912b_io_cec);   lt8912b_io_cec  = NULL; }
  if (lt8912b_io_main) { esp_lcd_del_panel_io(lt8912b_io_main);  lt8912b_io_main = NULL; }

  // Delete DSI bus last (panel depends on it)
  if (lt8912b_dsi_bus) { esp_lcd_del_dsi_bus(lt8912b_dsi_bus);   lt8912b_dsi_bus = NULL; }

  wledmm_display_w = 0;
  wledmm_display_h = 0;
}

// Initialise (or re-initialise) the full HDMI display stack for the given mode index.
// Safe to call after hdmi_display_deinit(); also called from setup() on first boot.
static void hdmi_display_init(int mode) {
  busNetworkDummyMode = true;

  // LDO for MIPI DSI PHY — acquired once, kept on for the lifetime of the app
  static esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
  if (!ldo_mipi_phy) {
    USER_PRINTLN("MIPI DSI PHY Power on");
    esp_ldo_channel_config_t ldo_cfg = { .chan_id = 3, .voltage_mv = 2500 };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy));
  }

  esp_lcd_dsi_bus_config_t bus_config = {};
  bus_config.bus_id         = 0;
  bus_config.num_data_lanes = 2;
  bus_config.phy_clk_src    = (mipi_dsi_phy_clock_source_t)4; // MIPI_DSI_PHY_CLK_SRC_DEFAULT

  hdmi_dpi_config_t timing = {};
  hdmi_get_dpi_config((hdmi_mode_t)mode, bus_config.num_data_lanes, timing);
  wledmm_display_w = timing.width;
  wledmm_display_h = timing.height;
  bus_config.lane_bit_rate_mbps = timing.lane_bit_rate_mbps;

  USER_PRINTLN("Initialize MIPI DSI bus");
  ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &lt8912b_dsi_bus));

  esp_lcd_dpi_panel_config_t dpi_config = {};
  dpi_config.dpi_clk_src                    = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
  dpi_config.dpi_clock_freq_mhz             = timing.dpi_clock_freq_mhz;
  dpi_config.virtual_channel                = 0;
  dpi_config.in_color_format                = LCD_COLOR_FMT_RGB888;
  dpi_config.out_color_format               = LCD_COLOR_FMT_RGB888;
  dpi_config.num_fbs                        = 2;
  dpi_config.video_timing.h_size            = timing.width;
  dpi_config.video_timing.v_size            = timing.height;
  dpi_config.video_timing.hsync_front_porch = timing.hsync_front_porch;
  dpi_config.video_timing.hsync_pulse_width = timing.hsync_pulse_width;
  dpi_config.video_timing.hsync_back_porch  = timing.hsync_back_porch;
  dpi_config.video_timing.vsync_front_porch = timing.vsync_front_porch;
  dpi_config.video_timing.vsync_pulse_width = timing.vsync_pulse_width;
  dpi_config.video_timing.vsync_back_porch  = timing.vsync_back_porch;
  dpi_config.flags.use_dma2d                = true;
  dpi_config.flags.disable_lp               = true;

  // Three I2C handles for LT8912B register access (main, CEC-DSI, AVI)
  esp_lcd_panel_io_handle_t io_main = NULL, io_cec = NULL, io_avi = NULL;
  esp_lcd_panel_io_i2c_config_t io_cfg_main = {};
  io_cfg_main.dev_addr                    = LT8912B_IO_I2C_MAIN_ADDRESS;
  io_cfg_main.control_phase_bytes         = 1;
  io_cfg_main.dc_bit_offset               = 0;
  io_cfg_main.lcd_cmd_bits                = 8;
  io_cfg_main.lcd_param_bits              = 8;
  io_cfg_main.flags.disable_control_phase = 1;
  io_cfg_main.scl_speed_hz                = 400000;
  esp_lcd_panel_io_i2c_config_t io_cfg_cec = io_cfg_main;
  io_cfg_cec.dev_addr = LT8912B_IO_I2C_CEC_ADDRESS;
  esp_lcd_panel_io_i2c_config_t io_cfg_avi = io_cfg_main;
  io_cfg_avi.dev_addr = LT8912B_IO_I2C_AVI_ADDRESS;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(global_i2c_bus_handle, &io_cfg_main, &io_main));
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(global_i2c_bus_handle, &io_cfg_cec,  &io_cec));
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(global_i2c_bus_handle, &io_cfg_avi,  &io_avi));
  lt8912b_io_main = io_main;
  lt8912b_io_cec  = io_cec;
  lt8912b_io_avi  = io_avi;

  lt8912b_vendor_config_t vendor_config = {
    .video_timing = ESP_LCD_LT8912B_VIDEO_TIMING_1280x720_60Hz(),
    .mipi_config = {
      .dsi_bus    = lt8912b_dsi_bus,
      .dpi_config = &dpi_config,
      .lane_num   = 2,
    },
  };
  vendor_config.video_timing.hfp         = timing.hsync_front_porch;
  vendor_config.video_timing.hs          = timing.hsync_pulse_width;
  vendor_config.video_timing.hbp         = timing.hsync_back_porch;
  vendor_config.video_timing.vfp         = timing.vsync_front_porch;
  vendor_config.video_timing.vs          = timing.vsync_pulse_width;
  vendor_config.video_timing.vbp         = timing.vsync_back_porch;
  vendor_config.video_timing.hact        = timing.width;
  vendor_config.video_timing.htotal      = timing.hsync_total;
  vendor_config.video_timing.vact        = timing.height;
  vendor_config.video_timing.vtotal      = timing.vsync_total;
  vendor_config.video_timing.h_polarity  = 1;
  vendor_config.video_timing.v_polarity  = 0;
  vendor_config.video_timing.vic         = timing.vic;
  vendor_config.video_timing.aspect_ratio = timing.aspect_ratio;
  vendor_config.video_timing.pclk_mhz   = timing.dpi_clock_freq_mhz;

  const esp_lcd_panel_dev_config_t panel_config = {
    .reset_gpio_num = -1,
    .rgb_ele_order  = (lcd_rgb_element_order_t)LCD_RGB_ELEMENT_ORDER_RGB,
    .bits_per_pixel = WLEDMM_DISPLAY_DEPTH,
    .vendor_config  = &vendor_config,
  };
  esp_lcd_panel_lt8912b_io_t lt8912b_io = { .main = io_main, .cec_dsi = io_cec, .avi = io_avi };

  USER_PRINTLN("Installing LT8912B HDMI bridge driver");
  ESP_ERROR_CHECK(esp_lcd_new_panel_lt8912b(&lt8912b_io, &panel_config, &panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
  USER_PRINTF("HDMI bridge init: %dx%d @ %dHz (mode %d: %s)\n",
    timing.width, timing.height, timing.fps, mode, hdmi_mode_names[mode]);
  {
    uint8_t r_c1 = lt8912b_read_reg(io_main, 0xC1);
    uint8_t r_b2 = lt8912b_read_reg(io_main, 0xB2);
    USER_PRINTF("LT8912B regs: 0xC1=0x%02x (HPD=%d) 0xB2=0x%02x (%s)\n",
      r_c1, (r_c1 >> 7) & 1, r_b2, (r_b2 & 1) ? "HDMI" : "DVI");
  }

  {
    const uint32_t timeout_ms = 5000, poll_ms = 200;
    uint32_t elapsed = 0;
    bool ready = false;
    USER_PRINT("Waiting for HDMI link");
    while (elapsed < timeout_ms) {
      ready = esp_lcd_panel_lt8912b_is_ready((esp_lcd_panel_t*)panel_handle);
      if (ready) break;
      USER_PRINT(".");
      vTaskDelay(pdMS_TO_TICKS(poll_ms));
      elapsed += poll_ms;
    }
    USER_PRINTF(ready ? " OK (%ums)\n" : " TIMEOUT — no monitor detected, continuing anyway\n", elapsed);
  }

  void* fb0_ptr = NULL;
  ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel_handle, 1, &fb0_ptr));
  display_framebuffer = (uint8_t*)fb0_ptr;
  if (!display_framebuffer) { USER_PRINTLN("FATAL: no display framebuffer!"); while (1) vTaskDelay(1); }
  USER_PRINTF("Display framebuffer at: %p (DPI-internal)\n", display_framebuffer);

  const size_t ppa_align = 4096;
  const size_t bytes_per_pixel = WLEDMM_DISPLAY_DEPTH / 8;
  const size_t fb_aligned_size = ((size_t)timing.width * timing.height * bytes_per_pixel + ppa_align - 1) & ~(ppa_align - 1);
  ppa_framebuffer = (uint8_t*)heap_caps_aligned_alloc(ppa_align, fb_aligned_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!ppa_framebuffer) { USER_PRINTLN("FATAL: no PPA framebuffer!"); while (1) vTaskDelay(1); }
  memset(ppa_framebuffer, 0, fb_aligned_size);
  USER_PRINTF("PPA framebuffer: %p (%u B)\n", ppa_framebuffer, (unsigned)fb_aligned_size);

  // Startup test pattern: RGB colour bands
  for (uint16_t y = 0; y < timing.height; y++) {
    uint8_t band = (y * 6) / timing.height;
    uint8_t r = (band == 0 || band == 3 || band == 4) ? 255 : 0;
    uint8_t g = (band == 1 || band == 3 || band == 5) ? 255 : 0;
    uint8_t b = (band == 2 || band == 4 || band == 5) ? 255 : 0;
    for (uint16_t x = 0; x < timing.width; x++) {
      uint32_t off = ((uint32_t)y * timing.width + x) * bytes_per_pixel;
      display_framebuffer[off + 0] = b;
      display_framebuffer[off + 1] = g;
      display_framebuffer[off + 2] = r;
    }
  }

  myFramebuffer.setPsram(true);
  buttonFramebuffer.setPsram(true);
  myFramebuffer.setColorDepth(WLEDMM_DISPLAY_DEPTH);
  buttonFramebuffer.setColorDepth(WLEDMM_DISPLAY_DEPTH);
  if (!myFramebuffer.createSprite(timing.width, 100))
    { USER_PRINTLN("Failed to allocate myFramebuffer sprite!"); while (1) vTaskDelay(1); }

  const int cols = WLEDMM_DISPLAY_BUTTONS_COLS, padding = 5;
  int rows = ((int)WLEDMM_DISPLAY_BUTTONS + cols - 1) / cols;
  if (rows < 1) rows = 1;
  int rectWidth  = (timing.width - (cols + 1) * padding) / cols;
  int rectHeight = (int)((float)rectWidth * 8.0f / (float)cols);
  int btnFbH     = rows * rectHeight + (rows + 1) * padding;
  if (!buttonFramebuffer.createSprite(timing.width, btnFbH))
    { USER_PRINTLN("Failed to allocate buttonFramebuffer sprite!"); while (1) vTaskDelay(1); }
  USER_PRINTF("Sprites allocated: main=%dx100, buttons=%dx%d\n", timing.width, timing.width, btnFbH);

  hdmi_current_mode = mode;
}

// Runtime display dimensions — set at init from WLEDMM_DISPLAY_MODE, replacing compile-time W/H defines
uint16_t wledmm_display_w = 0;
uint16_t wledmm_display_h = 0;
#define WLEDMM_DISPLAY_W   wledmm_display_w
#define WLEDMM_DISPLAY_H   wledmm_display_h
#define WLEDMM_DISPLAY_DEPTH 24  // RGB888 always for HDMI

#endif // CONFIG_IDF_TARGET_ESP32P4 && WLEDMM_DISPLAY_MODE

#ifdef SOC_USB_OTG_SUPPORTED
  #ifndef CONFIG_USB_HOST_HW_BUFFER_BIAS_BALANCED
    #error "USB Hardware Buffer Bias must be set to 'Balanced' via USB-OTG or CONFIG_USB_HOST_HW_BUFFER_BIAS_BALANCED=y."
    // This is likely to be fixed later. 
    // You could also comment out the #error and try (untested):
    #undef CONFIG_USB_HOST_HW_BUFFER_BIAS_IN
    #undef CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT
    #define CONFIG_USB_HOST_HW_BUFFER_BIAS_BALANCED 1
  #endif
  #include <dirent.h>
  #include "usb/usb_host.h"
  #include "usb/msc_host_vfs.h"
  #include "ImageCacheManager.h"
  
  #define MNT_PATH "/usb"     // Base mount path prefix, devices will be mounted as /usb0, /usb1, /usb2...
  #define MAX_MSC_DEVICES  CONFIG_FATFS_VOLUME_COUNT 

  #include "esp_vfs_fat.h"
  #include "sdmmc_cmd.h"
  #include "driver/sdmmc_host.h"
  #include "driver/gpio.h"
  #include <sys/stat.h>
  // SOC_SDMMC_HOST_SUPPORTED needs to be checked for SDMMC cards support.
  
  #define MOUNT_POINT "/sdcard"
  #define SD_POWER_PIN GPIO_NUM_45

  static sdmmc_card_t* card = NULL;

  static void sdcard_power_on(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SD_POWER_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(SD_POWER_PIN, 0);  // P-FET: low = on
    vTaskDelay(pdMS_TO_TICKS(50));    // let power stabilize
  }

  static void sdcard_power_off(void) {
    gpio_set_level(SD_POWER_PIN, 1);  // P-FET: high = off
  }

  esp_err_t mount_sdcard(void) {
    sdcard_power_on();

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    #if CONFIG_ESP_HOSTED_SDIO_SLOT == 0
    host.slot = SDMMC_HOST_SLOT_1;
    #elif CONFIG_ESP_HOSTED_SDIO_SLOT == 1
    host.slot = SDMMC_HOST_SLOT_0;
    #else
    host.slot = SDMMC_HOST_SLOT_0;
    #endif

    sdmmc_slot_config_t slot_config = {
        .clk = GPIO_NUM_43,
        .cmd = GPIO_NUM_44,
        .d0 = GPIO_NUM_39,
        .d1 = GPIO_NUM_40,
        .d2 = GPIO_NUM_41,
        .d3 = GPIO_NUM_42,
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP,
    };

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
      USER_PRINTF("Mount failed : %s\n", esp_err_to_name(ret));
      sdcard_power_off();
      return ret;
    }

    sdmmc_card_print_info(stdout, card);
    USER_PRINTF("Mounted at %s\n", MOUNT_POINT);
    return ESP_OK;
  }

  esp_err_t unmount_sdcard(void) {
    if (card == NULL) {
      return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    if (ret == ESP_OK) {
      card = NULL;
      sdcard_power_off();
      USER_PRINTLN("Unmounted");
    }
    return ret;
  }

  bool is_sdcard_mounted(void) {
    return card != NULL;
  }

  typedef struct {
    uint8_t usb_addr;                     /*!< USB device address */
    msc_host_device_handle_t msc_device;  /*!< Handle of the MSC device */
    msc_host_vfs_handle_t vfs_handle;     /*!< VFS handle assigned to the MSC device */
  } msc_dev_entry_t;

  static msc_dev_entry_t *msc_devices[MAX_MSC_DEVICES] = {0};

  static QueueHandle_t app_queue;

  typedef struct {
    enum {
      APP_QUIT,                // Signals request to exit the application
      APP_DEVICE_CONNECTED,    // USB device connect event
      APP_DEVICE_DISCONNECTED, // USB device disconnect event
    } id;
    union {
      uint8_t new_dev_address; // Address of new USB device for APP_DEVICE_CONNECTED event
      msc_host_device_handle_t device_handle; // Handle of removed USB device for APP_DEVICE_DISCONNECTED event
    } data;
  } app_message_t;

  static inline int find_free_slot(void)
  {
    for (int i = 0; i < MAX_MSC_DEVICES; i++) {
      if (msc_devices[i] == NULL) {
        return i;
      }
    }
    return -1;
  }

  static esp_err_t allocate_new_msc_device(const app_message_t *msg, int *out_slot)
  {
      int slot = find_free_slot();
      if (slot < 0) {
          ESP_LOGE(TAG, "No free slots for new MSC device (max %d)", MAX_MSC_DEVICES);
          return ESP_ERR_NOT_FOUND;
      }

      msc_devices[slot] = (msc_dev_entry_t *)calloc(1, sizeof(msc_dev_entry_t));

      if (!msc_devices[slot]) {
          ESP_LOGE(TAG, "Failed to allocate memory for new MSC device entry");
          return ESP_ERR_NO_MEM;
      }
      esp_err_t err = msc_host_install_device(msg->data.new_dev_address, &msc_devices[slot]->msc_device);
      if (err != ESP_OK) {
          ESP_LOGE(TAG, "msc_host_install_device failed: %s", esp_err_to_name(err));
          free(msc_devices[slot]);
          msc_devices[slot] = NULL;
          return err;
      }

      msc_devices[slot]->usb_addr = msg->data.new_dev_address;

      const esp_vfs_fat_mount_config_t mount_config = {
          .format_if_mount_failed = false,
          .max_files = 3,
          .allocation_unit_size = 8192,
      };

      char mount_path[16];
      snprintf(mount_path, sizeof(mount_path), MNT_PATH "%d", slot);

      err = msc_host_vfs_register(msc_devices[slot]->msc_device, mount_path, &mount_config, &msc_devices[slot]->vfs_handle);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_vfs_register failed: %s", esp_err_to_name(err));
        // Just call the uninstall function. Don't check the error in a way that aborts.
        // You can log its return value if you want, but the program must continue.
        esp_err_t uninstall_err = msc_host_uninstall_device(msc_devices[slot]->msc_device);
        if (uninstall_err != ESP_OK) {
          ESP_LOGW(TAG, "msc_host_uninstall_device failed during cleanup: %s", esp_err_to_name(uninstall_err));
        }
        free(msc_devices[slot]);
        msc_devices[slot] = NULL;
        // Return the original error that started this cleanup process
        return err;
      }

      *out_slot = slot;
      return ESP_OK;
  }

  static int find_slot_by_handle(msc_host_device_handle_t handle)
  {
      for (int i = 0; i < MAX_MSC_DEVICES; i++) {
          if (msc_devices[i] && msc_devices[i]->msc_device == handle) {
              return i;
          }
      }
      return -1;
  }

  static void free_msc_device(int slot)
  {
      if (slot < 0 || slot >= MAX_MSC_DEVICES || !msc_devices[slot]) {
          ESP_LOGE(TAG, "Invalid slot index for MSC device deallocation");
          return;
      }

      if (msc_devices[slot]->vfs_handle) {
          ESP_ERROR_CHECK(msc_host_vfs_unregister(msc_devices[slot]->vfs_handle));
      }
      if (msc_devices[slot]->msc_device) {
          ESP_ERROR_CHECK(msc_host_uninstall_device(msc_devices[slot]->msc_device));
      }

      free(msc_devices[slot]);
      msc_devices[slot] = NULL;
  }

  static void free_all_msc_devices(void)
  {
      for (int i = 0; i < MAX_MSC_DEVICES; i++) {
          if (msc_devices[i]) {
              free_msc_device(i);
          }
      }
  }

  static void gpio_cb(void *arg)
  {
      BaseType_t xTaskWoken = pdFALSE;
      app_message_t message = {
          .id = app_message_t::APP_QUIT,
      };

      if (app_queue) {
          xQueueSendFromISR(app_queue, &message, &xTaskWoken);
      }

      if (xTaskWoken == pdTRUE) {
          portYIELD_FROM_ISR();
      }
  }

  static inline int8_t find_usb_addr_by_handle(msc_host_device_handle_t handle)
  {
      for (int8_t i = 0; i < MAX_MSC_DEVICES; i++) {
          if (msc_devices[i] && msc_devices[i]->msc_device == handle) {
              return msc_devices[i]->usb_addr;
          }
      }
      return -1;
  }

  static void msc_event_cb(const msc_host_event_t *event, void *arg)
  {
      if (event->event == event->MSC_DEVICE_CONNECTED) {
          DEBUG_PRINTF("MSC device connected (usb_addr=%d)\n", event->device.address);
          app_message_t message = {};
          message.id = app_message_t::APP_DEVICE_CONNECTED;
          message.data.new_dev_address = event->device.address;

          xQueueSend(app_queue, &message, portMAX_DELAY);
      } else if (event->event == event->MSC_DEVICE_DISCONNECTED) {
          int usb_addr = find_usb_addr_by_handle(event->device.handle);
          if (usb_addr >= 0) {
              DEBUG_PRINTF("MSC device disconnected (usb_addr=%d)\n", usb_addr);
          } else {
              DEBUG_PRINTLN("MSC device disconnected, but failed to retrieve USB address");
          }
          app_message_t message = {};
          message.id = app_message_t::APP_DEVICE_DISCONNECTED;
          message.data.device_handle = event->device.handle;
          xQueueSend(app_queue, &message, portMAX_DELAY);
      }
  }

  static void print_device_info(msc_host_device_info_t *info)
  {
      const size_t megabyte = 1024 * 1024;
      uint64_t capacity = ((uint64_t)info->sector_size * info->sector_count) / megabyte;
      USER_PRINTF("USB Disk Capacity: %llu MB\n", capacity);
      // ESP_LOGE(TAG, "\t Sector size: %" PRIu32, info->sector_size);
      // ESP_LOGE(TAG, "\t Sector count: %" PRIu32, info->sector_count);
      // ESP_LOGE(TAG, "\t PID: 0x%04X", info->idProduct);
      // ESP_LOGE(TAG, "\t VID: 0x%04X", info->idVendor);
  }

  static void usb_task(void *args)
  {
      usb_host_config_t host_config = {};
      host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
      host_config.peripheral_map = BIT(0); // <--- this may be a bug of the current IDFv5.5 with USB High-Speed devices.

      // Bias Mode	  nptx_fifo_lines	  ptx_fifo_lines	rx_fifo_lines
      // Balanced	    256	              128	            512 (896 - 256 - 128)
      // IN-Biased	  64	              128	            704 (896 - 64 - 128) <--- does not work, but 896 seenms to be the correct total, confirmed by reading out the register.

      // Balanced values work: (works! and seem to match the IDF built with balanced defaults)
      // host_config.fifo_settings_custom.nptx_fifo_lines = 256;
      // host_config.fifo_settings_custom.ptx_fifo_lines = 128;
      // host_config.fifo_settings_custom.rx_fifo_lines = 512;

      // Testing a mid point: (marginal winner!) - THIS MAY BE FLAKEY? 
      // (tried a bunch off other ones too, this was the best - they were worse than balanced)
      //
      // host_config.fifo_settings_custom.nptx_fifo_lines = 128;
      // host_config.fifo_settings_custom.ptx_fifo_lines = 128;
      // host_config.fifo_settings_custom.rx_fifo_lines = 640;

      // If you need to know your on-SOC FIFO numbers this is the code - just for checking the register on the P4.
      // (The answer is 896, at least on all the current P4 devices I have.)
      //
      // periph_module_enable(PERIPH_UHCI_MODULE);
      // uint16_t fifo_depth_value = USB_DWC_HS.ghwcfg3_reg.dfifodepth;
      // USER_PRINTF("*** Extracted dfifodepth (fifo_size_lines): %u\n", fifo_depth_value);
      // periph_module_disable(PERIPH_UHCI_MODULE);

      ESP_ERROR_CHECK(usb_host_install(&host_config));

      const msc_host_driver_config_t msc_config = {
          .create_backround_task = true,
          .task_priority = 1, // xtaskcreate TroyHacks for finding later.
          .stack_size = 4096,
          .core_id = 0,
          .callback = msc_event_cb,
      };
      ESP_ERROR_CHECK(msc_host_install(&msc_config));

      bool has_clients = true;
      while (true) {
          uint32_t event_flags;
          usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

          // Release devices once all clients has deregistered
          if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
              has_clients = false;
              if (usb_host_device_free_all() == ESP_OK) {
                  break;
              };
          }
          if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE && !has_clients) {
              break;
          }
      }

      vTaskDelay(10); // Give clients some time to uninstall
      USER_PRINTLN("Deinitializing USB (This shouldn't happen)");
      ESP_ERROR_CHECK(usb_host_uninstall());
      vTaskDelete(NULL);
  }

  // static inline void show_list_files_all_devices(void) {
  //   // USER_PRINTF("ls command output for all connected devices:\n");
  //   for (int i = 0; i < MAX_MSC_DEVICES; i++) {
  //     if (msc_devices[i]) {
  //       char mount_path[16];
  //       snprintf(mount_path, sizeof(mount_path), MNT_PATH "%d", i);

  //       USER_PRINTF("Listing contents of %s:\n", mount_path);
  //       struct dirent* d;
  //       DIR* dh = opendir(mount_path);
  //       if (!dh) {
  //         USER_PRINTF("Failed to open directory: %s", mount_path);
  //         continue;
  //       }

  //       while ((d = readdir(dh)) != NULL) {
  //         USER_PRINTF("%s/%s\n", mount_path, d->d_name);
  //       }
  //       closedir(dh);
  //     }
  //   }
  // }

  #define APP_QUEUE_SIZE 5

#endif // SOC_USB_OTG_SUPPORTED

#ifdef ARDUINO_ARCH_ESP32
  #include "esp_ota_ops.h"
#endif
#warning WLED-MM is licensed under the EUPL-1.2. By installing WLED MM you implicitly accept the terms!

#if defined(ARDUINO_ARCH_ESP32) && defined(WLED_DISABLE_BROWNOUT_DET)
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#endif

#if defined(WLED_DEBUG) && defined(ARDUINO_ARCH_ESP32) // && !defined(CONFIG_IDF_TARGET_ESP32C6) && !defined(CONFIG_IDF_TARGET_ESP32P4)
#include "../tools/ESP32-Chip_info.hpp"
#endif

// #if defined(CONFIG_IDF_TARGET_ESP32P4)
// #include <WiFiGeneric.h>
// #endif

// WLEDMM some buildenv sanity checks

#ifdef ARDUINO_ARCH_ESP32 // ESP32
  #if !defined(ESP32)
    #error please fix your build environment. ESP32 is not defined.
  #endif
  #if defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266)
    #error please fix your build environment. ESP32 and ESP8266 are both defined.
  #endif
  // only one of ARDUINO_ARCH_ESP32S2, ARDUINO_ARCH_ESP32S3, ARDUINO_ARCH_ESP32C3 allowed
  #if defined(ARDUINO_ARCH_ESP32S3) && ( defined(ARDUINO_ARCH_ESP32S2) || defined(ARDUINO_ARCH_ESP32C3) )
    #error please fix your build environment. only one of ARDUINO_ARCH_ESP32S3, ARDUINO_ARCH_ESP32S2, ARDUINO_ARCH_ESP32C3 may be defined
  #endif
  #if defined(ARDUINO_ARCH_ESP32S2) && ( defined(ARDUINO_ARCH_ESP32S3) || defined(ARDUINO_ARCH_ESP32C3) )
    #error please fix your build environment. only one of ARDUINO_ARCH_ESP32S3, ARDUINO_ARCH_ESP32S2, ARDUINO_ARCH_ESP32C3 may be defined
  #endif
  #if defined(CONFIG_IDF_TARGET_ESP32) && ( defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32C3))
    #error please fix your build environment. only one CONFIG_IDF_TARGET may be defined
  #endif
  // make sure we have a supported CONFIG_IDF_TARGET_
  #if !defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S3) && !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(CONFIG_IDF_TARGET_ESP32C3)  && !defined(CONFIG_IDF_TARGET_ESP32C6) && !defined(CONFIG_IDF_TARGET_ESP32P4) && !defined(CONFIG_IDF_TARGET_ESP32C5) 
    #error please fix your build environment. No supported CONFIG_IDF_TARGET was defined
  #endif
  #if CONFIG_IDF_TARGET_ESP32_SOLO || CONFIG_IDF_TARGET_ESP32SOLO
    #warning ESP32 SOLO (single core) is not supported.
  #endif
  // only one of CONFIG_IDF_TARGET_ESP32, CONFIG_IDF_TARGET_ESP32S2, CONFIG_IDF_TARGET_ESP32S3, CONFIG_IDF_TARGET_ESP32C3 is allowed
  #if defined(CONFIG_IDF_TARGET_ESP32) && ( defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32C3))
    #error please fix your build environment. only one CONFIG_IDF_TARGET may be defined
  #endif
  #if defined(CONFIG_IDF_TARGET_ESP32S3) && ( defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32C3))
    #error please fix your build environment. only one CONFIG_IDF_TARGET may be defined
  #endif
  #if defined(CONFIG_IDF_TARGET_ESP32C3) && ( defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2))
    #error please fix your build environment. only one CONFIG_IDF_TARGET may be defined
  #endif

#else // 8266
  #if !defined(ARDUINO_ARCH_ESP8266) && !defined(ARDUINO_ARCH_ESP8265)
    #error please fix your build environment. Neither ARDUINO_ARCH_ESP8266 nor ARDUINO_ARCH_ESP32 are defined
  #else
    #if !defined(ESP8266) && !defined(ESP8265)
      #error please fix your build environment. ESP8266 is not defined.
    #endif
  #endif
#endif
// WLEDMM end


#if INCLUDE_xTaskGetHandle && defined(ARDUINO_ARCH_ESP32) && (defined(WLED_DEBUG) || defined(WLED_DEBUG_HEAP))
// WLEDMM stack debug tool - find async_tcp task, and queries it's free stack
static int wledmm_get_tcp_stacksize(void) {
  static TaskHandle_t tcp_taskHandle = NULL;                   // to store the task handle for later calls
  char * tcp_taskname = pcTaskGetTaskName(tcp_taskHandle);     // ask for name of the known task (to make sure we are still looking at the right one)

  if ((tcp_taskHandle == NULL) || (tcp_taskname == NULL) || (strncmp(tcp_taskname, "async_tcp", 9) != 0)) {
    tcp_taskHandle = xTaskGetHandle("async_tcp");              // need to look for the task by name. FreeRTOS docs say this is very slow, so we store the result for next time
    //DEBUG_PRINT(F("async_tcp task ")); DEBUG_PRINTLN( (tcp_taskHandle != NULL) ? F("found") : F("not found"));
  }

  if (tcp_taskHandle != NULL) return uxTaskGetStackHighWaterMark(tcp_taskHandle); // got it !!
  else return -1;
}
#endif

/*
 * Main WLED class implementation. Mostly initialization and connection logic
 */

WLED::WLED()
{
}

// turns all LEDs off and restarts ESP
void WLED::reset()
{
  briT = 0;
  #ifdef WLED_ENABLE_WEBSOCKETS
  ws.closeAll(1012);
#endif
  long dly = millis();
  while (millis() - dly < 450) {
    yield();        // enough time to send response to client
  }
  applyBri();
  USER_PRINTLN(F("\nWLED RESTART\n"));
  USER_FLUSH();   // WLEDMM: wait until Serial has completed sending buffered data
  ESP.restart();
}

#if defined(ARDUINO_ARCH_ESP32) && defined(WLEDMM_FASTPATH)
#define yield() {}  // WLEDMM yield() is completely unnecessary on esp32. See https://github.com/espressif/arduino-esp32/issues/1385
#endif

void background_loop_blocking(void* pvParameters) {
  for (;;) {


    if (doCloseFile) {
      if (xSemaphoreTake(busMutex, portMAX_DELAY)) {
        closeFile();
        xSemaphoreGive(busMutex);
      }
    }

    if (doSerializeConfig) {
      if (xSemaphoreTake(busMutex, portMAX_DELAY)) {
        serializeConfig();
        xSemaphoreGive(busMutex);
      }
    }

    //LED settings have been saved, re-init busses
    //This code block causes severe FPS drop on ESP32 with the original "if (busConfigs[0] != nullptr)" conditional. Investigate!
    if (doInitBusses) {
      if (xSemaphoreTake(busMutex, portMAX_DELAY)) {
        unsigned long waitStart = millis();                                             // WLEDMM: to avoid crash,
        // while (strip.isUpdating() && (millis() - waitStart < 250)) { yield(); delay(5); } // wait a bit until busses are idle (max 250ms)
        doInitBusses = false;
        DEBUG_PRINTLN(F("Re-init busses."));
        bool aligned = strip.checkSegmentAlignment(); //see if old segments match old bus(ses)
        busses.removeAll();
        uint32_t mem = 0;
        for (uint8_t i = 0; i < WLED_MAX_BUSSES + WLED_MIN_VIRTUAL_BUSSES; i++) {
          if (busConfigs[i] == nullptr) break;
          mem += BusManager::memUsage(*busConfigs[i]);
          if (mem <= MAX_LED_MEMORY) {
            busses.add(*busConfigs[i]);
          }
          delete busConfigs[i]; busConfigs[i] = nullptr;
        }
        strip.finalizeInit();
        busses.setBrightness(bri); // fix re-initialised bus' brightness
        loadLedmap = true;
        if (aligned) strip.makeAutoSegments();
        else strip.fixInvalidSegments();
        serializeConfig();
        xSemaphoreGive(busMutex);
      }
      if (e131Port == ARTNET_DEFAULT_PORT) {
        artnet.stop();
        artnet.begin(e131Universe, ARTNET_PRIORITY);
      }
    }

    if (loadLedmap) {
      if (strip.isMatrix && !strip.deserializeMap(loadedLedmap)) strip.setUpMatrix();
      // strip.enumerateLedmaps(); //WLEDMM TroyHacks: Removed in favor of cache maps.
      loadLedmap = false;
    }

    if (!realtimeMode || realtimeOverride || (realtimeMode && useMainSegmentOnly)) {

      if (xSemaphoreTake(busMutex, portMAX_DELAY)) {
        handlePlaylist();
        handlePresets();
        // usermods.loop2(); // nothing does this.
        xSemaphoreGive(busMutex);
      }

    }

    vTaskDelay(1);

  }
}

void background_loop_nonblocking(void* pvParameters) {
  for (;;) {
    #ifdef WLED_DEBUG
    // esp_log_level_set("*",ESP_LOG_VERBOSE);
    static unsigned long maxUsermodMillis = 0;
    static uint16_t avgUsermodMillis = 0;
    static unsigned long maxStripMillis = 0;
    static uint16_t avgStripMillis = 0;
    #endif

    static const UBaseType_t MY_BASE_PRIORITY = uxTaskPriorityGet(wled_main_task);
    static const UBaseType_t MY_MAX_PRIORITY = 3;
    static unsigned long stuckSince = 0;
    static uint8_t ws_reset_times = 0;
    
    if (uxTaskPriorityGet(wled_main_task) > MY_MAX_PRIORITY && eTaskGetState(wled_main_task) == eBlocked && (strip.getFps() > 0 && strip.getFps() < 8)) {
      
      if (stuckSince == 0) stuckSince = millis();

      if (millis() - stuckSince > 5000) {  // Stuck for 5+ seconds

        USER_PRINTF("wled_main_task priority = %d and strip.getFps() = %d\n", uxTaskPriorityGet(wled_main_task), strip.getFps());

        vTaskPrioritySet(wled_main_task, MY_BASE_PRIORITY);

        if (ws.count() > 0 && ws_reset_times < 3) {
          USER_PRINTF("Detected stuck state, resetting WebSockets... %d ws clients\n", ws.count());
          ws.closeAll();
          ws.cleanupClients();
          vTaskDelay(pdMS_TO_TICKS(100));
          ws.onEvent(wsEvent);
          stuckSince = 0;
          ws_reset_times++;
        } else {
          USER_PRINTF("Detected stuck state, re-initing interfaces. %d ws clients\n", ws.count());
          WLED::instance().initInterfaces();
          stuckSince = 0;
          ws_reset_times = 0;
        }
      }
    }
    
    handleTime();
    WLED::instance().handleConnection();
    #ifndef WLED_DISABLE_ESPNOW
    handleRemote();
    #endif
    handleSerial();
    #ifndef WLED_DISABLE_IMPROV_WIFISCAN
    handleImprovWifiScan();
    #endif

    handleNotifications();
    handleTransitions();

    #ifdef WLED_ENABLE_DMX
    handleDMXOutput();
    #endif
    #ifdef WLED_ENABLE_DMX_INPUT
    dmxInput.update();
    #endif
    // userLoop(); // this is 
    usermods.loop();
    #ifdef WLED_DEBUG
    unsigned long usermodMillis = millis();
    #endif

    #ifdef WLED_DEBUG
    usermodMillis = millis() - usermodMillis;
    avgUsermodMillis += usermodMillis;
    if (usermodMillis > maxUsermodMillis) maxUsermodMillis = usermodMillis;
    #endif

    handleIO();

    if (doReboot && !doInitBusses) { // if busses have to be inited & saved, wait until next iteration
      WLED::reset();
    }

    if (apActive) dnsServer.processNextRequest();
    
    if (!realtimeMode || realtimeOverride || (realtimeMode && useMainSegmentOnly)) {

      #ifndef WLED_DISABLE_OTA
      if (WLED_CONNECTED && aOtaEnabled && !otaLock && correctPIN) ArduinoOTA.handle();
      #endif

      handleNightlight();

    }

    if (lastMqttReconnectAttempt > millis()) { //millis() rolls over every 50 days
      rolloverMillis++;
      lastMqttReconnectAttempt = 0;
      ntpLastSyncTime = NTP_NEVER;  // force new NTP query
      strip.restartRuntime();
    }

    if (millis() - lastMqttReconnectAttempt > 30000 || lastMqttReconnectAttempt == 0) { // lastMqttReconnectAttempt==0 forces immediate broadcast
      lastMqttReconnectAttempt = millis();
      refreshNodeList(); // refresh WLED nodes list
      if (nodeBroadcastEnabled) sendSysInfoUDP();
    }

    // 15min PIN time-out
    if (strlen(settingsPIN) > 0 && millis() - lastEditTime > 900000) {
      correctPIN = false;
      createEditHandler(false);
    }

    // handleWs();

    #ifdef STATUSLED
    WLED::handleStatusLED();
    #endif

    #ifdef SOC_USB_OTG_SUPPORTED
    app_message_t msg;
    // Poll for messages without blocking
    if (xQueueReceive(app_queue, &msg, 0)) {
      switch (msg.id) {
      case 1: {
        int slot;
        if (allocate_new_msc_device(&msg, &slot) == ESP_OK) {
          // USER_PRINTLN("USB Disk Connected");
          msc_host_device_info_t info;
          ESP_ERROR_CHECK_WITHOUT_ABORT(msc_host_get_device_info(msc_devices[slot]->msc_device, &info));
          print_device_info(&info);
          // show_list_files_all_devices();
          USER_PRINTLN("ImageCache started");
          ImageCacheManager::getInstance().startPreload("/usb0");
        } else {
          USER_PRINTLN("USB operation failed. Try replugging?");
        }
        break;
      }

      case 2: {
        USER_PRINTLN("USB Device Disconnected");

        int slot = find_slot_by_handle(msg.data.device_handle);
        if (slot >= 0) {
          free_msc_device(slot);
        }
        break;
      }

      default:
        USER_PRINTF("Unknown USB Error message ID: %d\n", msg.id);
        break;
      }
    }
    #endif // SOC_USB_OTG_SUPPORTED

    toki.resetTick();

    vTaskDelay(1);

  }
}

void WLED::loop() { // loopTask
  #ifdef WLED_DEBUG
  // esp_log_level_set("*",ESP_LOG_VERBOSE);
  static unsigned long maxUsermodMillis = 0;
  static uint16_t avgUsermodMillis = 0;
  static unsigned long maxStripMillis = 0;
  static uint16_t avgStripMillis = 0;
  #endif

  if (!interfacesInited || strip.getBrightness() == 0) {
    taskYIELD();  // Just yield, don't sleep
  }
  
  if (!realtimeMode || realtimeOverride || (realtimeMode && useMainSegmentOnly)) {

    #ifdef WLED_DEBUG
    unsigned long stripMillis = millis();
    #endif

    if (!offMode || strip.isOffRefreshRequired()) {
      if (xSemaphoreTake(busMutex, 0)) {
        strip.service();
        xSemaphoreGive(busMutex);
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    #ifdef WLED_DEBUG
    stripMillis = millis() - stripMillis;
    avgStripMillis += stripMillis;
    if (stripMillis > maxStripMillis) maxStripMillis = stripMillis;
    #endif
  }

  if (e131Port == ARTNET_DEFAULT_PORT) {
    artnet.processNewFrame();

    if (realtimeMode == REALTIME_MODE_ARTNET && newArtNetData) {
      if (!offMode || strip.isOffRefreshRequired()) {
        if (xSemaphoreTake(busMutex, 0)) {
          strip.show();
          xSemaphoreGive(busMutex);
        }
      }
      newArtNetData = false;
    }
  }

#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(WLEDMM_DISPLAY_MODE) && defined(CONFIG_SOC_PPA_SUPPORTED)
  // HDMI blit — runs every loop regardless of realtimeMode or Art-Net input state
  if (display_framebuffer && panel_handle) {
    static uint32_t last_blit_us = 0;
    uint32_t now_us = esp_timer_get_time();
    if (1 || now_us - last_blit_us >= 33333) {  // ~30fps
      last_blit_us = now_us;
      if (xSemaphoreTake(busMutex, 0)) {
        Bus* bus = busses.getBus(0);
        uint8_t* busPixelData = bus ? bus->getPixelData() : nullptr;
        if (busPixelData) {
          const int ledW = SEGMENT.maxWidth  ? SEGMENT.maxWidth  : WLEDMM_DISPLAY_W;
          const int ledH = SEGMENT.maxHeight ? SEGMENT.maxHeight : WLEDMM_DISPLAY_H;
          // Use the full display area — no UI bars yet
          float scale  = min((float)WLEDMM_DISPLAY_W / ledW, (float)WLEDMM_DISPLAY_H / ledH);
          int scaledW  = (int)(ledW * scale);
          int scaledH  = (int)(ledH * scale);

          if (scale > 0.0f && scale <= 16.0f && scaledW > 0 && scaledH > 0) {
            static uint32_t blit_ok = 0, blit_fail = 0, blit_log_ms = 0;
            const size_t fb_size = (size_t)WLEDMM_DISPLAY_W * WLEDMM_DISPLAY_H * 3;

            ppa_srm_oper_config_t srm_cfg = {};
            srm_cfg.in.srm_cm          = PPA_SRM_COLOR_MODE_RGB888;
            srm_cfg.out.srm_cm         = PPA_SRM_COLOR_MODE_RGB888;
            srm_cfg.in.buffer          = busPixelData;
            srm_cfg.in.pic_w           = ledW;
            srm_cfg.in.pic_h           = ledH;
            srm_cfg.in.block_w         = ledW;
            srm_cfg.in.block_h         = ledH;
            srm_cfg.out.buffer         = display_framebuffer;
            srm_cfg.out.buffer_size    = fb_size;
            srm_cfg.out.pic_w          = WLEDMM_DISPLAY_W;
            srm_cfg.out.pic_h          = WLEDMM_DISPLAY_H;
            srm_cfg.out.block_offset_x = (WLEDMM_DISPLAY_W - scaledW) / 2;
            srm_cfg.out.block_offset_y = (WLEDMM_DISPLAY_H - scaledH) / 2;
            srm_cfg.scale_x            = scale;
            srm_cfg.scale_y            = scale;
            srm_cfg.rotation_angle     = PPA_SRM_ROTATION_ANGLE_0;
            srm_cfg.rgb_swap           = true;  // bus data is R,G,B; display expects R,G,B (no swap needed)
            srm_cfg.byte_swap          = false;
            srm_cfg.mode               = PPA_TRANS_MODE_BLOCKING;

            if (ESP_ERROR_CHECK_WITHOUT_ABORT(ppa_do_scale_rotate_mirror(ppa_srm_handle, &srm_cfg)) == ESP_OK) {
              blit_ok++;
            } else {
              blit_fail++;
            }

            // Diagnostic: every 5s print blit counts and first pixel of source & dest
            uint32_t now_ms = millis();
            if (now_ms - blit_log_ms >= 5000) {
              blit_log_ms = now_ms;
              {
                uint8_t r_c1 = lt8912b_io_main ? lt8912b_read_reg(lt8912b_io_main, 0xC1) : 0;
                uint8_t r_b2 = lt8912b_io_main ? lt8912b_read_reg(lt8912b_io_main, 0xB2) : 0;
                USER_PRINTF("HDMI blit ok=%u fail=%u src[0]=%02x%02x%02x dst[0]=%02x%02x%02x scale=%.2f %dx%d hpd=%d %s\n",
                  blit_ok, blit_fail,
                  busPixelData[0], busPixelData[1], busPixelData[2],
                  display_framebuffer[0], display_framebuffer[1], display_framebuffer[2],
                  scale, ledW, ledH,
                  (r_c1 >> 7) & 1,
                  (r_b2 & 1) ? "HDMI" : "DVI");
              }
              blit_ok = blit_fail = 0;
            }
          }
        }
        xSemaphoreGive(busMutex);
      }
    }

    // UI sprite blits — disabled until UI is ready
    static uint32_t last_ui_ms = 0;
    uint32_t now_ms = millis();
    if (false && (now_ms - last_ui_ms >= 1000 || update_screen)) {
      last_ui_ms = now_ms;
      if (myFramebuffer.getBuffer()) {
        ppa_srm_oper_config_t ui_cfg = {};
        ui_cfg.in.srm_cm    = PPA_SRM_COLOR_MODE_RGB888;
        ui_cfg.out.srm_cm   = PPA_SRM_COLOR_MODE_RGB888;
        ui_cfg.in.buffer    = (uint8_t*)myFramebuffer.getBuffer();
        ui_cfg.in.pic_w     = WLEDMM_DISPLAY_W;
        ui_cfg.in.pic_h     = 100;
        ui_cfg.in.block_w   = WLEDMM_DISPLAY_W;
        ui_cfg.in.block_h   = 100;
        ui_cfg.out.buffer      = ppa_framebuffer;
        ui_cfg.out.buffer_size = WLEDMM_DISPLAY_W * WLEDMM_DISPLAY_H * 3;
        ui_cfg.out.pic_w       = WLEDMM_DISPLAY_W;
        ui_cfg.out.pic_h       = WLEDMM_DISPLAY_H;
        ui_cfg.scale_x         = 1.0f;
        ui_cfg.scale_y         = 1.0f;
        ui_cfg.rotation_angle  = PPA_SRM_ROTATION_ANGLE_0;
        ui_cfg.mode            = PPA_TRANS_MODE_BLOCKING;
        ESP_ERROR_CHECK_WITHOUT_ABORT(ppa_do_scale_rotate_mirror(ppa_srm_handle, &ui_cfg));
      }
      if (update_screen && buttonFramebuffer.getBuffer()) {
        update_screen = false;
        ppa_srm_oper_config_t btn_cfg = {};
        btn_cfg.in.srm_cm          = PPA_SRM_COLOR_MODE_RGB888;
        btn_cfg.out.srm_cm         = PPA_SRM_COLOR_MODE_RGB888;
        btn_cfg.in.buffer          = (uint8_t*)buttonFramebuffer.getBuffer();
        btn_cfg.in.pic_w           = WLEDMM_DISPLAY_W;
        btn_cfg.in.pic_h           = buttonFramebuffer.height();
        btn_cfg.in.block_w         = WLEDMM_DISPLAY_W;
        btn_cfg.in.block_h         = buttonFramebuffer.height();
        btn_cfg.out.buffer         = ppa_framebuffer;
        btn_cfg.out.buffer_size    = WLEDMM_DISPLAY_W * WLEDMM_DISPLAY_H * 3;
        btn_cfg.out.pic_w          = WLEDMM_DISPLAY_W;
        btn_cfg.out.pic_h          = WLEDMM_DISPLAY_H;
        btn_cfg.out.block_offset_y = WLEDMM_DISPLAY_H - buttonFramebuffer.height();
        btn_cfg.scale_x            = 1.0f;
        btn_cfg.scale_y            = 1.0f;
        btn_cfg.rotation_angle     = PPA_SRM_ROTATION_ANGLE_0;
        btn_cfg.mode               = PPA_TRANS_MODE_BLOCKING;
        ESP_ERROR_CHECK_WITHOUT_ABORT(ppa_do_scale_rotate_mirror(ppa_srm_handle, &btn_cfg));
      }
    }
  }
#endif // CONFIG_IDF_TARGET_ESP32P4 && WLEDMM_DISPLAY_MODE && CONFIG_SOC_PPA_SUPPORTED

  #if defined(WLED_DEBUG) && !defined(WLED_DEBUG_HEAP) // DEBUG serial logging (every 30s)
  if (millis() - debugTime > 29999) {
    DEBUG_PRINTLN(F("---DEBUG INFO---"));
    DEBUG_PRINT(F("Name: "));       DEBUG_PRINTLN(serverDescription);
    DEBUG_PRINT(F("Runtime: "));       DEBUG_PRINTLN(millis());
    DEBUG_PRINT(F("Unix time: "));     toki.printTime(toki.getTime());
    DEBUG_PRINT(F("Free heap : "));     DEBUG_PRINTLN(ESP.getFreeHeap());
    DEBUG_PRINT(F("Free heap: "));     DEBUG_PRINTLN(ESP.getFreeHeap());
    //WLEDMM
    #ifdef ARDUINO_ARCH_ESP32
    DEBUG_PRINT(F("Avail heap: "));     DEBUG_PRINTLN(ESP.getMaxAllocHeap());
    DEBUG_PRINTF("%s min free stack %d\n", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL)); //WLEDMM
    #endif
    #if defined(ARDUINO_ARCH_ESP32) && defined(BOARD_HAS_PSRAM) && !defined(CONFIG_IDF_TARGET_ESP32P4)
    if (psramFound()) {  // OK use
      //DEBUG_PRINT(F("Total PSRAM: "));    DEBUG_PRINT(ESP.getPsramSize()/1024); DEBUG_PRINTLN("kB");
      DEBUG_PRINT(F("Free PSRAM : "));     DEBUG_PRINT(ESP.getFreePsram() / 1024); DEBUG_PRINTLN("kB");
      DEBUG_PRINT(F("Avail PSRAM: "));     DEBUG_PRINT(ESP.getMaxAllocPsram() / 1024); DEBUG_PRINTLN("kB");
      DEBUG_PRINT(F("PSRAM in use:")); DEBUG_PRINT(int(ESP.getPsramSize() - ESP.getFreePsram())); DEBUG_PRINTLN(F(" Bytes"));
    } else {
      //DEBUG_PRINTLN(F("No PSRAM"));
    }
    #endif
    // DEBUG_PRINT(F("Wifi state: "));      DEBUG_PRINTLN(WiFi.status());

    // if (WiFi.status() != lastWifiState) {
    //   wifiStateChangedTime = millis();
    // }
    // lastWifiState = WiFi.status();
    DEBUG_PRINT(F("State time: "));      DEBUG_PRINTLN(wifiStateChangedTime);
    DEBUG_PRINT(F("NTP last sync: "));   DEBUG_PRINTLN(ntpLastSyncTime);
    DEBUG_PRINT(F("Client IP: "));       DEBUG_PRINTLN(Network.localIP());
    if (loops > 0) { // avoid division by zero
      DEBUG_PRINT(F("Loops/sec: "));       DEBUG_PRINTLN(loops / 30);
      DEBUG_PRINT(F("UM time[ms]: "));     DEBUG_PRINT(avgUsermodMillis / loops); DEBUG_PRINT("/");DEBUG_PRINTLN(maxUsermodMillis);
      DEBUG_PRINT(F("Strip time[ms]: "));  DEBUG_PRINT(avgStripMillis / loops); DEBUG_PRINT("/"); DEBUG_PRINTLN(maxStripMillis);
    }
    strip.printSize();
    loops = 0;
    maxUsermodMillis = 0;
    maxStripMillis = 0;
    avgUsermodMillis = 0;
    avgStripMillis = 0;
    debugTime = millis();
    DEBUG_PRINTLN(F("---END OF DEBUG INFO---"));
  }
  loops++;
  #endif
  #ifdef WLED_DEBUG_HEAP
  if (millis() - debugTime > 4999) { // WLEDMM: Special case for debugging heap faster
    DEBUG_PRINT(F("*** Free heap: "));     DEBUG_PRINT(heap_caps_get_free_size(0x1800));
    DEBUG_PRINT(F("\tLargest free block: "));     DEBUG_PRINT(heap_caps_get_largest_free_block(0x1800));
    DEBUG_PRINT(F(" *** \t\tArduino min free stack: ")); DEBUG_PRINT(uxTaskGetStackHighWaterMark(NULL));
    #if INCLUDE_xTaskGetHandle
    DEBUG_PRINT(F("   TCP min free stack: ")); DEBUG_PRINT(wledmm_get_tcp_stacksize());
    #endif
    DEBUG_PRINTLN(F(" ***"));
  }
  #endif        // WLED_DEBUG_HEAP

  if (xSemaphoreTake(busMutex, 0)) {
    handleWs();
    xSemaphoreGive(busMutex);
  }

  #if WLED_WATCHDOG_TIMEOUT > 0
  esp_task_wdt_reset();
  #endif

  // vTaskDelay(1);

} // end main loop
  
#if defined(ARDUINO_ARCH_ESP32) && defined(WLEDMM_FASTPATH)
#undef yield  // WLEDMM restore yield()
#endif

void WLED::enableWatchdog() {
#if WLED_WATCHDOG_TIMEOUT > 0
  #if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t wdtConfig;
  wdtConfig.timeout_ms = WLED_WATCHDOG_TIMEOUT * 1000;  // convert to milliseconds
  wdtConfig.idle_core_mask = (1 << CONFIG_FREERTOS_NUMBER_OF_CORES) - 1;
  wdtConfig.trigger_panic = false; // TroyHacks P4: Stop panics temporarily until we sort the WDT out.
  esp_err_t watchdog = esp_task_wdt_reconfigure(&wdtConfig);
  #else
  esp_err_t watchdog = esp_task_wdt_init(WLED_WATCHDOG_TIMEOUT, true);
  #endif
  DEBUG_PRINT(F("Watchdog enabled: "));
  if (watchdog == ESP_OK) {
    DEBUG_PRINTLN(F("OK"));
  } else {
    DEBUG_PRINTLN(watchdog);
    return;
  }
  esp_task_wdt_add(NULL);
#endif
}

void WLED::disableWatchdog() {
  #if WLED_WATCHDOG_TIMEOUT > 0
  DEBUG_PRINTLN(F("Watchdog: disabled"));
  esp_task_wdt_delete(NULL);
  #endif
}

int retry_num=0;

static void wifi_event_handler(void* event_handler_arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {

  if (event_base == WIFI_EVENT) {

    if (event_id == WIFI_EVENT_STA_START) {
      USER_PRINTLN("Event: WiFi Started");
      ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
      if (!interfacesInited) interfacesInited = false;
      wifi_is_connected = false;
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
      USER_PRINTLN("Event: WiFi Connected");
      if (!interfacesInited) interfacesInited = false;
      wifi_is_connected = false;
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
      wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*)event_data;
      USER_PRINTF("Disconnected from %s, reason: %d\n", event->ssid, event->reason);
      USER_PRINTLN("Event: WiFi Lost Connection");
      interfacesInited = false;
      wifi_is_connected = false;
      if (s_retry_num < 5) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
        s_retry_num++;
        USER_PRINTF("Event Action: Retry to connect to the AP %d\n", s_retry_num);
      }
      if (apBehavior == AP_BEHAVIOR_NO_CONN && !apActive) {
        USER_PRINTLN("Connection lost, restarting AP");
        WLED::initAP(false);
      }
    } else if (event_id == WIFI_EVENT_HOME_CHANNEL_CHANGE) {
      // USER_PRINTLN("Event: WiFi HOME CHANNEL CHANGED");
    } else if (event_id == WIFI_EVENT_STA_STOP) {
      USER_PRINTLN("Event: WiFi Stopped");
      interfacesInited = false;
      wifi_is_connected = false;
    } else if (event_id == WIFI_EVENT_AP_START) {
      USER_PRINTLN("Event: SoftAP Started");
      ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
      interfacesInited = false;
      wifi_is_connected = false;
      g_ap_client_count = 0; // Reset count when AP starts
    } else if (event_id == WIFI_EVENT_AP_STOP) {
      USER_PRINTLN("Event: SoftAP Stopped");
      interfacesInited = false;
      wifi_is_connected = false;
      g_ap_client_count = 0; // Reset count when AP stops
    } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
      USER_PRINTLN("Event: AP Client Connected");
      portENTER_CRITICAL(&g_ap_client_mux);
      g_ap_client_count++;
      portEXIT_CRITICAL(&g_ap_client_mux);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
      USER_PRINTLN("Event: AP Client Disconnected");
      portENTER_CRITICAL(&g_ap_client_mux);
      g_ap_client_count--;
      portEXIT_CRITICAL(&g_ap_client_mux);
    } else {
      USER_PRINTF("Event: WiFi threw unidentified code %d\n", event_id);
    }

  } else if (event_base == IP_EVENT) {

    if (event_id == IP_EVENT_STA_GOT_IP) {
      USER_PRINTLN("Event: WiFi Got IP");
      interfacesInited = false;
      wifi_is_connected = true;
      showWelcomePage = false;
      ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
      s_retry_num = 0;

      // Always stop DNS hijacking once we have internet
      dnsServer.stop();
      if (apActive) USER_PRINTLN("Stopped captive portal DNS");

      // Handle AP based on behavior setting
      switch (apBehavior) {
      case AP_BEHAVIOR_BOOT_NO_CONN:
      case AP_BEHAVIOR_NO_CONN:
        // Shut down AP since we now have a connection
        esp_wifi_set_mode(WIFI_MODE_STA);
        apActive = false;
        if (apActive) USER_PRINTLN("Disabled AP (connection established)");
        break;

      case AP_BEHAVIOR_ALWAYS:
        // Keep AP running, but captive portal DNS is already stopped
        USER_PRINTLN("Keeping AP active (ALWAYS mode)");
        break;

      case AP_BEHAVIOR_BUTTON_ONLY:
        // Shouldn't normally get here with AP active, but handle it
        if (apActive) {
          esp_wifi_set_mode(WIFI_MODE_STA);
          apActive = false;
        }
        break;
      }
    }
  }
}

#ifdef WLED_USE_ETHERNET

static void eth_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
  if (event_id == ETHERNET_EVENT_CONNECTED) {
    USER_PRINTLN("Event: Ethernet Link Up");
    eth_link_up = true;  // Physical link is up (Layer 2)
    eth_is_connected = false;
  } else if (event_id == ETHERNET_EVENT_DISCONNECTED) {
    esp_netif_t* eth_netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    esp_netif_set_route_prio(eth_netif, 0);
    USER_PRINTLN("Event: Ethernet Link Down");
    eth_link_up = false;  // Physical link is down
    eth_is_connected = false;
    USER_PRINT("IP Address is now http://");
    USER_PRINTLN(Network.localIP());
    MDNS.end();
    escapedMac = Network.getEscapedMac();
    sprintf_P(cmDNS, PSTR("wled-%*s"), 6, escapedMac.c_str() + 6);
    MDNS.begin(cmDNS);
    USER_PRINTF("mDNS started: http://%s.local\n", cmDNS); // WLEDMM
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("wled", "tcp", 80);
    MDNS.addServiceTxt("wled", "tcp", "mac", escapedMac.c_str());
  } else if (event_id == ETHERNET_EVENT_START) {
    eth_link_up = false;  // Link not up yet
    eth_is_connected = false;
    char hostname[25];
    prepareHostname(hostname);
    Network.setHostname(hostname);
    // USER_PRINTLN("Event: Ethernet Started");
  } else {
    USER_PRINTF("Event: Ethernet Undeclared Event %d\n", event_id);
  }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  esp_netif_t* eth_netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
  esp_netif_set_route_prio(eth_netif, 200);
  eth_is_connected = true;
  interfacesInited = false;
}
#endif

void print_wifi_protocols(const char* prefix, uint16_t protocols) {
  USER_PRINTF("%s: ", prefix);

  bool first = true;

  if (protocols & WIFI_PROTOCOL_11B) {
    USER_PRINTF("802.11b");
    first = false;
  }
  if (protocols & WIFI_PROTOCOL_11G) {
    if (!first) USER_PRINTF(" | ");
    USER_PRINTF("802.11g");
    first = false;
  }
  if (protocols & WIFI_PROTOCOL_11N) {
    if (!first) USER_PRINTF(" | ");
    USER_PRINTF("802.11n");
    first = false;
  }
  if (protocols & WIFI_PROTOCOL_11A) {
    if (!first) USER_PRINTF(" | ");
    USER_PRINTF("802.11a");
    first = false;
  }
  if (protocols & WIFI_PROTOCOL_11AC) {
    if (!first) USER_PRINTF(" | ");
    USER_PRINTF("802.11ac");
    first = false;
  }
  if (protocols & WIFI_PROTOCOL_11AX) {
    if (!first) USER_PRINTF(" | ");
    USER_PRINTF("802.11ax");
    first = false;
  }
  if (protocols & WIFI_PROTOCOL_LR) {
    if (!first) USER_PRINTF(" | ");
    USER_PRINTF("LR");
    first = false;
  }

  if (first) {
    USER_PRINTF("None");
  }

  USER_PRINTF(" (0x%02x)\n", protocols);
}

const char* wifi_band_mode_to_string(wifi_band_mode_t mode) {
  switch (mode) {
  case WIFI_BAND_MODE_2G_ONLY:   return "2.4GHz Only";
  case WIFI_BAND_MODE_5G_ONLY:   return "5GHz Only";
  case WIFI_BAND_MODE_AUTO:      return "Auto (2.4GHz/5GHz)";
  default:                        return "Unknown";
  }
}

#ifdef ENABLE_VL53L8CX
void IRAM_ATTR tofISR() {
  vl53l8cx_NewDataReady = true;
}

void sensorTask(void* parameter) {
  for (;;) {
    // Check if data is ready (polling or interrupt)
    if (vl53l8cx_NewDataReady) {

      // Read data into a LOCAL buffer first (slow, blocking part)
      VL53L8CX_ResultsData tempResults;
      sensor_vl53l8cx_top.get_ranging_data(&tempResults);

      // Quickly copy to global variable (protected)
      xSemaphoreTake(vl53l8cxMutex, portMAX_DELAY);
      memcpy(&vl53l8cx_Results, &tempResults, sizeof(VL53L8CX_ResultsData));
      vl53l8cx_data_available = true;
      xSemaphoreGive(vl53l8cxMutex);
    }

    // Prevent watchdog starvation
    vTaskDelay(5 / portTICK_PERIOD_MS);
  }
}
#endif

void WLED::setup() {

  #ifdef WLED_DEBUG
  // esp_log_level_set("*",ESP_LOG_VERBOSE);
  #endif 

  #if defined(ARDUINO_ARCH_ESP32) && defined(WLED_DISABLE_BROWNOUT_DET)
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); //disable brownout detection
  #endif

  pinMode(hardwareRX, INPUT_PULLDOWN); delay(1);        // suppress noise in case RX pin is floating (at low noise energy) - see issue #3128

  #ifdef WLED_BOOTUPDELAY
  delay(WLED_BOOTUPDELAY); // delay to let voltage stabilize, helps with boot issues on some setups
  #endif
  #if defined(CONFIG_IDF_TARGET_ESP32P4) || defined(ARDUINO_USB_CDC_ON_BOOT)
  Serial.begin(115200);
  #else
  Serial.begin(115200, SERIAL_8N1, SOC_RX0, SOC_TX0, false, 20000UL, 120U);
  #endif

#if !defined(WLEDMM_NO_SERIAL_WAIT) || defined(WLED_DEBUG)
  if (!Serial) delay(1000); // WLEDMM make sure that Serial has initalized
#else
  if (!Serial) delay(300);  // just a tiny wait to avoid problems later when acessing serial
#endif
  Serial.flush();
#ifdef ARDUINO_ARCH_ESP32
  #if defined(WLED_DEBUG) && (defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32C3) || ARDUINO_USB_CDC_ON_BOOT)
  if (!Serial) delay(2500);  // WLEDMM allow CDC USB serial to initialise (WLED_DEBUG only)
  #endif
  #if ARDUINO_USB_CDC_ON_BOOT || ARDUINO_USB_MODE
    #if ARDUINO_USB_CDC_ON_BOOT && (defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32P4))
    //  WLEDMM avoid "hung devices" when USB_CDC is enabled; see https://github.com/espressif/arduino-esp32/issues/9043
    Serial.setTxTimeoutMs(0);    // potential side-effect: incomplete debug output, with missing characters whenever TX buffer is full.
    #endif
#if !defined(WLEDMM_NO_SERIAL_WAIT) || defined(WLED_DEBUG)
  if (!Serial) delay(2500);  // WLEDMM: always allow CDC USB serial to initialise
  if (Serial) Serial.println("wait 1");  // waiting a bit longer ensures that a  debug messages are shown in serial monitor
  if (!Serial) delay(2500);
  if (Serial) Serial.println("wait 2");
  if (!Serial) delay(2500);

  if (Serial) Serial.flush(); // WLEDMM
#endif

  //Serial.setTimeout(350); // WLEDMM: don't change timeout, as it causes crashes later
  // WLEDMM: redirect debug output to HWCDC
  #if ARDUINO_USB_CDC_ON_BOOT && (defined(WLED_DEBUG) || defined(SR_DEBUG))
  Serial0.setDebugOutput(false);
  Serial.setDebugOutput(true);
  #endif
  // WLEDMM don't touch serial timeout when we use CDC USB or tinyUSB
  #else // "standard" serial-to-USB chip
  if (Serial) Serial.setTimeout(50);  // WLEDMM - only when serial is initialized
  #endif
  #else  // 8266
  if (Serial) Serial.setTimeout(50);  // WLEDMM - only when serial is initialized
  #endif
  
  init_math();  // WLEDMM: pre-calculate some lookup tables

  bool fsinit = false;
  USER_PRINTLN(F("Mounting FS ..."));
  #ifdef ARDUINO_ARCH_ESP32
  fsinit = WLED_FS.begin(true);
  #else
  fsinit = WLED_FS.begin();
  #endif
  if (!fsinit) {
    USER_PRINTLN(F("Mount FS failed!"));  // WLEDMM
    errorFlag = ERR_FS_BEGIN;
  } else {
    USER_PRINTLN(F("Mount FS succeeded.")); // WLEDMM
  }
  DEBUG_PRINTLN(F("Reading host name"));
  char hostname[33] = "wled-xxxx";
  getHostnameFromConfig(hostname, sizeof(hostname));
  DEBUG_PRINTF("hostname == %s\n", hostname);
  esp_log_level_set("i2c", ESP_LOG_NONE);

  #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5,0,0)
    #if !defined(WLED_USE_ETHERNET_ONLY)
      esp_err_t ret = nvs_flash_init();
      if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS is corrupt or full. This is unrecoverable.
        USER_PRINTLN("NVS partition corrupt! Erasing and restarting...");
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_flash_erase());
        esp_restart();
      }
      ESP_ERROR_CHECK_WITHOUT_ABORT(ret); // Check for other errors
      USER_PRINTLN("NVS flash initialized.");
      ESP_ERROR_CHECK(esp_event_loop_create_default());
      #if defined(CONFIG_IDF_TARGET_ESP32P4)
        ESP_ERROR_CHECK(esp_hosted_init());
        ESP_ERROR_CHECK(esp_hosted_connect_to_slave());
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_restore());
        esp_hosted_coprocessor_fwver_t c6_fw_version;
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_hosted_get_coprocessor_fwversion(&c6_fw_version));
        if (c6_fw_version.major1 >= 2 && c6_fw_version.major1 <= 1000) {
          USER_PRINTF("ESP-Hosted C6 Firmware is version %d.%d.%d\n", c6_fw_version.major1, c6_fw_version.minor1, c6_fw_version.patch1);
        } else {
          USER_PRINTF("ESP-Hosted C6 Firmware is older than verion 2.15.12\n");
        }
        esp_err_t check = ota_littlefs_perform(true);
        if (check == ESP_HOSTED_SLAVE_OTA_COMPLETED) {
          esp_err_t ret = esp_hosted_slave_ota_activate();
          if (ret == ESP_OK) {
            USER_PRINTLN("Slave will reboot with new firmware");
            USER_PRINTLN("********* Restarting host to avoid sync issues *********");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
          } else {
            USER_PRINTF("Failed to activate OTA: %s\n", esp_err_to_name(ret));
          }
        } else if (check == ESP_HOSTED_SLAVE_OTA_NOT_REQUIRED) {
          USER_PRINTLN("WiFi CoProcessor doesn't need upgrading!");
        }
      #else
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_restore());
      #endif
        esp_netif_init();
        sta_netif = esp_netif_create_default_wifi_sta();
        ap_netif = esp_netif_create_default_wifi_ap();
        wifi_init_config_t wifi_initiation = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&wifi_initiation);

        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

        wifi_country_t country = {
          .cc = "US",
          .schan = 1,
          .nchan = 14,
          .max_tx_power = 20,
          .policy = WIFI_COUNTRY_POLICY_MANUAL
        };
        esp_wifi_set_country(&country);

        esp_wifi_set_mode(WIFI_MODE_STA);
        // ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());

        // // Wait for WiFi to be fully ready
        vTaskDelay(pdMS_TO_TICKS(1000));

        // // Stop any auto-connection attempt
        // esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
        USER_PRINTLN("Checking WiFi Stuff");

        wifi_band_mode_t band_mode;
        esp_wifi_get_band_mode(&band_mode);
        USER_PRINTF("Band mode: %s\n", wifi_band_mode_to_string(band_mode));

        wifi_protocols_t protocols;
        esp_wifi_get_protocols(WIFI_IF_STA, &protocols);
        print_wifi_protocols("2.4GHz protocols before set:", protocols.ghz_2g);
        if (band_mode != WIFI_BAND_MODE_2G_ONLY) print_wifi_protocols("5GHz protocols before set:", protocols.ghz_5g);

        wifi_country_t country_check;
        esp_wifi_get_country(&country_check);
        USER_PRINTF("Country: %.2s, channels %d-%d\n", country_check.cc, country_check.schan, country_check.schan + country_check.nchan - 1);

        wifi_protocols_t xprotocols;
        wifi_bandwidths_t bw_config;

        if (band_mode != WIFI_BAND_MODE_2G_ONLY) {
          xprotocols = {
            .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N,
            .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N
          };
          ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_protocols(WIFI_IF_STA, &xprotocols));
          bw_config = {
            .ghz_2g = WIFI_BW_HT40,
            .ghz_5g = WIFI_BW_HT40,
          };
          esp_err_t err = esp_wifi_set_bandwidths(WIFI_IF_STA, &bw_config);
          USER_PRINTF("Set bandwidths result: %d (%s)\n", err, esp_err_to_name(err));
        } else {
          ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
          esp_err_t err = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40);
          USER_PRINTF("Set bandwidth result: %d (%s)\n", err, esp_err_to_name(err));
        }

        esp_wifi_get_protocols(WIFI_IF_STA, &xprotocols);

        print_wifi_protocols("2.4GHz protocols after set:", xprotocols.ghz_2g);
        if (band_mode != WIFI_BAND_MODE_2G_ONLY) print_wifi_protocols("5GHz protocols after set:", xprotocols.ghz_5g);

        #ifdef WLEDMM_SHOW_WIFI_SCAN
        // Scan to see what networks are visible
        wifi_scan_config_t scan_config = {
          .ssid = NULL,
          .bssid = NULL,
          .channel = 0,
          .show_hidden = true,
          .scan_type = WIFI_SCAN_TYPE_ACTIVE,
          .scan_time = {
            .active = {.min = 100, .max = 300 },
          }
        };
        
        USER_PRINTLN("Starting scan...");
        esp_err_t scan_err = esp_wifi_scan_start(&scan_config, true);
        USER_PRINTF("Scan returned: %d\n", scan_err);

        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);

        USER_PRINTF("Found %d APs:\n", ap_count);
        if (ap_count > 0) {
          wifi_ap_record_t* ap_list = (wifi_ap_record_t*)malloc(ap_count * sizeof(wifi_ap_record_t));
          esp_wifi_scan_get_ap_records(&ap_count, ap_list);

          for (int i = 0; i < ap_count; i++) {
            USER_PRINTF("  %-24s CH:%3d RSSI:%d %s\n",
              ap_list[i].ssid,
              ap_list[i].primary,
              ap_list[i].rssi,
              ap_list[i].primary >= 36 ? "(5GHz)" : "(2.4GHz)");
          }
          free(ap_list);
        }
        #endif
        #endif



    #ifdef WLED_USE_ETHERNET
      // Initialize TCP/IP network interface

      #if defined(WLED_USE_ETHERNET_ONLY)
      // With coexistence you don't need these again (can crash!)
      ESP_ERROR_CHECK(esp_netif_init());
      ESP_ERROR_CHECK(esp_event_loop_create_default());
      #endif

      // Create default Ethernet interface
      // esp_netif_inherent_config_t my_eth_base = _g_esp_netif_inherent_eth_config;
      // my_eth_base.route_prio = 200;
      // esp_netif_config_t cfg = {
      //   .base = &my_eth_base,
      //   .driver = NULL,
      //   .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
      // };
      esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
      // base_cfg.route_prio = 200;

      esp_netif_config_t cfg = {
          .base = &base_cfg,
          .driver = NULL,
          .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
      };

      eth_netif = esp_netif_new(&cfg);
      assert(eth_netif);

      #ifdef CONFIG_ETH_USE_ESP32_EMAC
        // Initialize Ethernet driver
        eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
        esp32_emac_config.smi_gpio.mdc_num = 31;
        esp32_emac_config.smi_gpio.mdio_num = 52;
        eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
        eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
        phy_config.phy_addr = 1; // Set PHY address
        phy_config.reset_gpio_num = 51; // Set PHY reset GPIO number

        esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
        esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);

        esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
        
        ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth_handle));

#ifdef WLED_ENABLE_COLORLIGHT
        // ColorLight mode: Skip IP stack attachment for raw Layer 2 operation
        USER_PRINTLN(F("[Ethernet] ColorLight mode: Skipping IP stack (raw Ethernet only)"));
#else
        ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));
        Network.setHostname(hostname);
#endif

        // Start Ethernet driver
        ESP_ERROR_CHECK(esp_eth_start(eth_handle));

        // Register event handler for Ethernet events
        ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));

#ifndef WLED_ENABLE_COLORLIGHT
        // Only register IP events if not using ColorLight
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));
#endif
      #elif defined(CONFIG_ETH_USE_SPI_ETHERNET) && defined(CONFIG_ETH_SPI_ETHERNET_W5500)
        USER_PRINTLN("Setup W5500 for ESP32-C5 (IDF v5)...");

        // --- PIN DEFINITIONS (Native IO MUX for SPI2 on C5) ---
        gpio_num_t w5500_miso = GPIO_NUM_2;
        gpio_num_t w5500_mosi = GPIO_NUM_7;
        gpio_num_t w5500_sclk = GPIO_NUM_6;
        gpio_num_t w5500_csss = GPIO_NUM_10;
        gpio_num_t w5500_rset = GPIO_NUM_24;
        // Note: We are using Polling Mode, so Interrupt Pin is not used here.

        // Define Host
        #define W5500_HOST_ID SPI2_HOST

        // 1. SPI Bus Config
        // Note: Strict order is required for C++ compilers
        spi_bus_config_t buscfg = {
            .mosi_io_num = w5500_mosi,
            .miso_io_num = w5500_miso,
            .sclk_io_num = w5500_sclk,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .data4_io_num = -1,
            .data5_io_num = -1,
            .data6_io_num = -1,
            .data7_io_num = -1,
            .max_transfer_sz = 4092,
        };

        ESP_ERROR_CHECK(spi_bus_initialize(W5500_HOST_ID, &buscfg, SPI_DMA_CH_AUTO));

        // 2. SPI Device Config
        spi_device_interface_config_t spi_devcfg = {
            .mode = 0,
            .clock_speed_hz = 12 * 1000 * 1000, // Reduced to 12MHz for stability
            .spics_io_num = w5500_csss,
            .queue_size = 1
        };

        // 3. W5500 Config (Polling Mode)
        eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(W5500_HOST_ID, &spi_devcfg);
        w5500_config.int_gpio_num = -1;  // Disable Interrupts (Polling Mode)
        w5500_config.poll_period_ms = 10; // Poll every 10ms

        eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
        mac_config.sw_reset_timeout_ms = 1000;

        eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
        phy_config.reset_gpio_num = w5500_rset;
        phy_config.phy_addr = -1;

        // 4. Create Driver Instances
        esp_eth_mac_t* mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
        esp_eth_phy_t* phy = esp_eth_phy_new_w5500(&phy_config);

        esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);

        // 5. Install Driver
        ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth_handle));

#ifdef WLED_ENABLE_COLORLIGHT
        // ColorLight mode: Skip IP stack attachment for raw Layer 2 operation
        USER_PRINTLN(F("[Ethernet] ColorLight mode: Skipping IP stack (raw Ethernet only)"));
#else
        // 6. Attach to Netif (This AUTOMATICALLY registers the default handlers in v5)
        ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));
#endif

        // 7. Start Driver
        ESP_ERROR_CHECK(esp_eth_start(eth_handle));

        // 8. VERIFY MAC ADDRESS (Debugging Step)
        // If this prints 00:00:00..., your SPI MISO line is likely broken.
        uint8_t mac_addr[6] = { 0 };
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        USER_PRINTF("W5500 MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
          mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

        // 9. Register Application Events
        ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
#ifndef WLED_ENABLE_COLORLIGHT
        // Only register IP events if not using ColorLight
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));
#endif
      #endif
    #endif
  #endif

  //Serial0.setDebugOutput(false);
  #if CORE_DEBUG_LEVEL || defined(WLED_DEBUG_HEAP) || defined(WLED_DEBUG)
  Serial.setDebugOutput(true);  // enables kernel debug messages on Serial
  #endif
  USER_FLUSH(); delay(100);
  USER_PRINTLN();
  USER_PRINT(F("---WLED "));
  #ifdef WLEDMM_FASTPATH
  USER_PRINT("=FASTPATH= ");
  #endif
  USER_PRINT(versionString);
  USER_PRINT(" ");
  USER_PRINT(VERSION);
  USER_PRINTLN(F(" INIT---"));
  #ifdef WLED_RELEASE_NAME
  USER_PRINTF(" WLEDMM_%s %s, build %s.\n", versionString, releaseString, TOSTRING(VERSION)); // WLEDMM specific
  #endif

  const esp_partition_t *running_partition = esp_ota_get_running_partition();
  USER_PRINTF("Running from: %s which is %u bytes and type %u subtype %u at address %x\n",running_partition->label,running_partition->size,running_partition->type,running_partition->subtype,running_partition->address);

  DEBUG_PRINT(F("esp32 "));
  DEBUG_PRINTLN(ESP.getSdkVersion());
  #if defined(ESP_ARDUINO_VERSION)
    //DEBUG_PRINTF(F("arduino-esp32  0x%06x\n"), ESP_ARDUINO_VERSION);
    DEBUG_PRINTF("arduino-esp32 v%d.%d.%d\n", int(ESP_ARDUINO_VERSION_MAJOR), int(ESP_ARDUINO_VERSION_MINOR), int(ESP_ARDUINO_VERSION_PATCH));  // availeable since v2.0.0
  #else
    DEBUG_PRINTLN(F("arduino-esp32 v1.0.x\n"));  // we can't say in more detail.
  #endif

  USER_PRINT(F("CPU:   ")); USER_PRINT(ESP.getChipModel());
  #if !defined (CONFIG_IDF_TARGET_ESP32C5)
  USER_PRINT(F(" rev.")); USER_PRINT(ESP.getChipRevision());
  #endif
  USER_PRINT(F(", ")); USER_PRINT(ESP.getChipCores()); USER_PRINT(F(" core(s)"));
  USER_PRINT(F(", ")); USER_PRINT(ESP.getCpuFreqMHz()); USER_PRINTLN(F("MHz."));

  // WLEDMM begin
  delay(20); USER_FLUSH(); // drain serial output buffers
  USER_PRINT(F("CPU    "));
  esp_reset_reason_t resetReason = getRestartReason();
  USER_PRINT(restartCode2InfoLong(resetReason));
  USER_PRINT(F(" (code "));
  USER_PRINT((int)resetReason);
  USER_PRINT(F("). "));
  int core0code = getCoreResetReason(0);
  int core1code = getCoreResetReason(1);
  USER_PRINTF("Core#0 %s (%d)", resetCode2Info(core0code).c_str(), core0code);
  if (core1code > 0) {USER_PRINTF("; Core#1 %s (%d)", resetCode2Info(core1code).c_str(), core1code);}
  USER_PRINTLN(F("."));
  if ((core0code > 1) && (core0code <= 20) && (core0code != 3) && (core0code != 12) && (core0code != 14)) errorFlag = ERR_SYS_REBOOT; // abnormal reboot
  if ((resetReason >= 4) && (resetReason < 10)) errorFlag = ERR_SYS_REBOOT; // abnormal reboot (crash, brownout, watchdog, etc)
  if ((resetReason == ESP_RST_BROWNOUT) || (core0code == 15)) errorFlag = ERR_SYS_BROWNOUT; // brownout detected
  // WLEDMM end

  USER_PRINT(F("FLASH: ")); USER_PRINT((ESP.getFlashChipSize()/1024)/1024);
  // USER_PRINT(F("MB, Mode ")); USER_PRINT(ESP.getFlashChipMode());
  #ifdef WLED_DEBUG
  // switch (ESP.getFlashChipMode()) {
  //   // missing: Octal modes
  //   case FM_QIO:  DEBUG_PRINT(F(" (QIO)")); break;
  //   case FM_QOUT: DEBUG_PRINT(F(" (QOUT)"));break;
  //   case FM_DIO:  DEBUG_PRINT(F(" (DIO)")); break;
  //   case FM_DOUT: DEBUG_PRINT(F(" (DOUT)"));break;
  //   #if defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_ESPTOOLPY_FLASHMODE_OPI
  //     case FM_FAST_READ: DEBUG_PRINT(F(" (OPI)"));break;
  //   #else
  //     case FM_FAST_READ: DEBUG_PRINT(F(" (fast_read)"));break;
  //   #endif
  //   case FM_SLOW_READ: DEBUG_PRINT(F(" (slow_read)"));break;
  //   default: break;
  // }
  #endif
  USER_PRINT(F(", speed ")); USER_PRINT(ESP.getFlashChipSpeed()/1000000);USER_PRINTLN(F("MHz."));
  
  #if defined(WLED_DEBUG) && defined(ARDUINO_ARCH_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32C6) && !defined(CONFIG_IDF_TARGET_ESP32P4)
  showRealSpeed();
  #endif

  DEBUG_PRINT(F("heap ")); DEBUG_PRINTLN(ESP.getFreeHeap());

  #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)  // unfortunately not available in older framework versions
  DEBUG_PRINT(F("\nArduino  max stack  ")); DEBUG_PRINTLN(getArduinoLoopTaskStackSize());
  #endif
  DEBUG_PRINTF("%s min free stack %d\n", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL)); //WLEDMM

#if defined(BOARD_HAS_PSRAM) || defined(CONFIG_ESPTOOLPY_FLASHMODE_OPI)
  //psramInit(); //WLEDMM?? softhack007: not sure if explicit init is really needed ... lets disable it here and see if that works
  #if defined(CONFIG_IDF_TARGET_ESP32S3)
    #if CONFIG_ESPTOOLPY_FLASHMODE_OPI || (CONFIG_SPIRAM_MODE_OCT && defined(BOARD_HAS_PSRAM))
      // S3: reserve GPIO 33-37 for "octal" PSRAM
      managed_pin_type pins[] = { {33, true}, {34, true}, {35, true}, {36, true}, {37, true} };
      pinManager.allocateMultiplePins(pins, sizeof(pins)/sizeof(managed_pin_type), PinOwner::SPI_RAM);
    #endif
  #elif defined(CONFIG_IDF_TARGET_ESP32S2)
  // S2: reserve GPIO 26-32 for PSRAM (may fail due to isPinOk() but that will also prevent other allocation)
  //managed_pin_type pins[] = { {26, true}, {27, true}, {28, true}, {29, true}, {30, true}, {31, true}, {32, true} };
  //pinManager.allocateMultiplePins(pins, sizeof(pins)/sizeof(managed_pin_type), PinOwner::SPI_RAM);
  #elif defined(CONFIG_IDF_TARGET_ESP32C3)
  // C3: reserve GPIO 12-17 for PSRAM (may fail due to isPinOk() but that will also prevent other allocation)
  //managed_pin_type pins[] = { {12, true}, {13, true}, {14, true}, {15, true}, {16, true}, {17, true} };
  //pinManager.allocateMultiplePins(pins, sizeof(pins)/sizeof(managed_pin_type), PinOwner::SPI_RAM);
  #elif defined(CONFIG_IDF_TARGET_ESP32P4)
  // ESP32-P4 peripherals are hidden from GPIO map, including PSRAM - so we don't need to further hide them.
  // Note: 3.3V LDOs for GPIO>36 expansion are not needed for the Olimex ESP32-P4-PC HDMI build.
  // MIPI DSI PHY LDO (chan_id=3, 2500mV) is acquired later during display init.
  #else
  // GPIO16/GPIO17 reserved for SPI RAM
  managed_pin_type pins[] = { {16, true}, {17, true} };
  pinManager.allocateMultiplePins(pins, sizeof(pins)/sizeof(managed_pin_type), PinOwner::SPI_RAM);
  #endif
  
  #ifdef SOC_RX0
  pinManager.allocatePin(SOC_RX0, false, PinOwner::DebugOut);
  #endif
  #ifdef SOC_TX0
  pinManager.allocatePin(SOC_TX0, true, PinOwner::DebugOut);
  #endif

  #if defined(BOARD_HAS_PSRAM) && (defined(WLED_USE_PSRAM) || defined(WLED_USE_PSRAM_JSON))       // WLEDMM
  if (psramFound()) {  // OK use
    DEBUG_PRINT(F("Total PSRAM: ")); DEBUG_PRINT(ESP.getPsramSize()/1024); DEBUG_PRINTLN("kB");
    DEBUG_PRINT(F("Free PSRAM : ")); DEBUG_PRINT(ESP.getFreePsram()/1024); DEBUG_PRINTLN("kB");
  }
  #else
    DEBUG_PRINTLN(F("PSRAM not used."));
  #endif
#endif
#ifdef CONFIG_SOC_PPA_SUPPORTED
  ESP_ERROR_CHECK(ppa_register_client(&ppa_blend_config, &ppa_blend_handle));
  ESP_ERROR_CHECK(ppa_register_client(&ppa_fill_config, &ppa_fill_handle));
  ESP_ERROR_CHECK(ppa_register_client(&ppa_srm_config, &ppa_srm_handle));
  ESP_ERROR_CHECK(ppa_register_client(&ppa_srm_config, &preview_ppa_srm_handle));
  ESP_ERROR_CHECK(jpeg_new_decoder_engine(&decode_eng_cfg, &jpgd_handle));
#endif
#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(SOC_USB_OTG_SUPPORTED)
  DEBUG_PRINTLN("Initializing USB Host...");
  app_queue = xQueueCreate(APP_QUEUE_SIZE, sizeof(app_message_t));
  if (!app_queue) {
    DEBUG_PRINTLN( "Failed to create USB Host app_queue");
    return;
  }
  xTaskCreatePinnedToCore(usb_task, "usb_task", 4096, NULL, 2, NULL, 0);
  DEBUG_PRINTLN("Setup complete. Waiting for USB Host events.");
#endif

  if ((strncmp("ESP32-PICO", ESP.getChipModel(), 10) == 0) || (strncmp("ESP32-U4WDH", ESP.getChipModel(), 11) == 0))
  { // WLEDMM detect pico board and esp32-mini1 board at runtime
    // special handling for PICO-D4: gpio16+17 are in use for onboard SPI FLASH (not PSRAM)
    managed_pin_type pins[] = { {16, true}, {17, true} };
    pinManager.allocateMultiplePins(pins, sizeof(pins)/sizeof(managed_pin_type), PinOwner::SPI_RAM);
  }

  //DEBUG_PRINT(F("LEDs inited. heap usage ~"));
  //DEBUG_PRINTLN(heapPreAlloc - ESP.getFreeHeap());
  USER_FLUSH();  // WLEDMM flush buffer now, before anything time-critical is started.

  pinManager.manageDebugTXPin();

#ifdef WLED_ENABLE_DMX //reserve GPIO2 as hardcoded DMX pin
  pinManager.allocatePin(2, true, PinOwner::DMX);
#endif

#if defined(BOARD_HAS_PSRAM)
  if (psramFound()) {  // OK use
    DEBUG_PRINT(F("\nfree heap ")); DEBUG_PRINTLN(ESP.getFreeHeap());
    USER_PRINTLN(F("JSON gabage collection (initial)."));
    // doc.garbageCollect();   // WLEDMM experimental - this seems to move the complete doc[] into PSRAM TroyHacks FIXME this is just to make IDF 5.5 work
	  USER_PRINT(F("PSRAM in use:")); USER_PRINT(int(ESP.getPsramSize() - ESP.getFreePsram())); USER_PRINTLN(F(" Bytes."));
    DEBUG_PRINT(F("free heap ")); DEBUG_PRINTLN(ESP.getFreeHeap());
  }
#endif

// WLEDMM experimental: support for single neoPixel on Adafruit boards
#if 0
  //#ifdef PIN_NEOPIXEL
  //pinManager.allocatePin(PIN_NEOPIXEL, true, PinOwner::BusDigital);
  //#endif
  #ifdef NEOPIXEL_POWER
    pinManager.allocatePin(NEOPIXEL_POWER, true, PinOwner::Relay);  // just to ensure this GPIO will not get used for other purposes
    pinMode(NEOPIXEL_POWER, OUTPUT);
    digitalWrite(NEOPIXEL_POWER, HIGH);
  #endif
  #ifdef NEOPIXEL_I2C_POWER
    pinManager.allocatePin(NEOPIXEL_I2C_POWER, true, PinOwner::Relay);  // just to ensure this GPIO will not get used for other purposes
    pinMode(NEOPIXEL_I2C_POWER, OUTPUT);
    digitalWrite(NEOPIXEL_I2C_POWER, HIGH);
  #endif
#endif

  USER_PRINTLN();
  DEBUG_PRINTLN(F("Registering usermods ..."));
  registerUsermods();

  DEBUG_PRINT(F("heap ")); DEBUG_PRINTLN(ESP.getFreeHeap());
  #ifdef ARDUINO_ARCH_ESP32
    DEBUG_PRINTF("%s min free stack %d\n", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL)); //WLEDMM
  #endif

  for (uint8_t i=1; i<WLED_MAX_BUTTONS; i++) btnPin[i] = -1;

#ifdef WLED_ADD_EEPROM_SUPPORT
  else deEEP();
#else
  initPresetsFile();
  buildPresetCache();
#endif
  updateFSInfo();

  USER_PRINT(F("done Mounting FS; "));
  USER_PRINT(((fsBytesTotal-fsBytesUsed)/1024)); USER_PRINTLN(F(" kB free.\n"));

  escapedMac = Network.getEscapedMac();

  WLED_SET_AP_SSID(); // otherwise it is empty on first boot until config is saved

  DEBUG_PRINTLN(F("Reading config"));
  deserializeConfigFromFS();
  onload_loadedLedmap = loadedLedmap;

  #if defined(SOC_SDMMC_HOST_SUPPORTED)
  err_t sdcarderr = mount_sdcard();
  if (sdcarderr == ESP_OK) {
    USER_PRINT("Backup of LittleFS to SD Card... ");
    backupLittleFStoSD();
    USER_PRINTLN("Done!");
    USER_PRINT("Starting SD card preload... ");
    ImageCacheManager::getInstance().startPreload("/sdcard");
    ImageCacheManager::getInstance().waitUntilIdle();
    USER_PRINTLN("Done!");
  }
  #endif 

#if defined(STATUSLED) && STATUSLED>=0
  if (!pinManager.isPinAllocated(STATUSLED)) {
    // NOTE: Special case: The status LED should *NOT* be allocated.
    //       See comments in handleStatusLed().
    pinMode(STATUSLED, OUTPUT);
  }
#endif
  #if defined(CONFIG_IDF_TARGET_ESP32P4)
  esp_ldo_dump(stdout);
  #endif
  DEBUG_PRINTLN(F("Initializing strip"));
  beginStrip();
  DEBUG_PRINT(F("heap ")); DEBUG_PRINTLN(ESP.getFreeHeap());

  USER_PRINTLN(F("\nUsermods setup ..."));
  userSetup();
  usermods.setup();
  DEBUG_PRINT(F("heap ")); DEBUG_PRINTLN(ESP.getFreeHeap());

  if (strcmp(clientSSID, DEFAULT_CLIENT_SSID) == 0) {
    #ifndef WLED_USE_ETHERNET_ONLY
    showWelcomePage = true;
    #endif
  }

  // fill in unique mdns default
  sprintf_P(cmDNS, PSTR("wled-%*s"), 6, escapedMac.c_str() + 6);

#ifndef WLED_DISABLE_OTA
  if (aOtaEnabled) {
    ArduinoOTA.onStart([]() {
      WLED::instance().disableWatchdog();
      DEBUG_PRINTLN(F("Start ArduinoOTA"));
    });
    ArduinoOTA.onError([](ota_error_t error) {
      // reenable watchdog on failed update
      WLED::instance().enableWatchdog();
    });
    if (strlen(cmDNS) > 0)
      ArduinoOTA.setHostname(cmDNS);
  }
#endif
#ifdef WLED_ENABLE_DMX
  initDMXOutput();
#endif
#ifdef WLED_ENABLE_DMX_INPUT
  dmxInput.init(dmxInputReceivePin, dmxInputTransmitPin, dmxInputEnablePin, dmxInputPort);
#endif

  // HTTP server page init
  DEBUG_PRINTLN(F("initServer"));
  initServer();
  DEBUG_PRINT(F("heap ")); DEBUG_PRINTLN(ESP.getFreeHeap());
  #ifdef ARDUINO_ARCH_ESP32
  DEBUG_PRINT(pcTaskGetTaskName(NULL)); DEBUG_PRINT(F(" free stack ")); DEBUG_PRINTLN(uxTaskGetStackHighWaterMark(NULL));
  #endif

  // Seed FastLED random functions with an esp random value, which already works properly at this point.
  uint32_t seed32 = esp_random();
  seed32 ^= random(0, INT32_MAX);  // WLEDMM some extra entropy (for older frameworks where esp_ramdom alone might be too predictable after startup)

  random16_set_seed((uint16_t)((seed32 & 0xFFFF) ^ (seed32 >> 16)));

  #if WLED_WATCHDOG_TIMEOUT > 0
  enableWatchdog();
  #endif

  #if defined(WLED_DISABLE_BROWNOUT_DET)
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 1); //enable brownout detector
  #endif
  
  #ifdef ARDUINO_RUNNING_CORE
    DEBUG_PRINTF("Arduino core=%d (loop is now on core #%d)\n", int(ARDUINO_RUNNING_CORE), int(xPortGetCoreID()));
  #endif
    #ifdef ARDUINO_EVENT_RUNNING_CORE
    DEBUG_PRINTF("Arduino Event core=%d\n", int(ARDUINO_EVENT_RUNNING_CORE));
    #endif

    // WLEDMM : dump GPIO infos (experimental, UI integration pending)
    //#ifdef WLED_DEBUG
    USER_PRINTLN(F("\nGPIO\t| Assigned to\t\t| Info"));
    USER_PRINTLN(F("--------|-----------------------|------------"));
    for (int pinNr = 0; pinNr < WLED_NUM_PINS; pinNr++) { // 49 = highest PIN on ESP32-S3
      #if defined(CONFIG_IDF_TARGET_ESP32S3)
      if ((pinManager.isPinOk(pinNr, false)) || (pinNr > 18 && pinNr < 21)) {  // softhack007: list USB pins
        #else
      if (pinManager.isPinOk(pinNr, false)) {
        #endif
        //if ((!pinManager.isPinAllocated(pinNr)) && (pinManager.getPinSpecialText(pinNr).length() == 0)) continue;      // un-comment to hide no-name,unused GPIO pins
        bool is_inOut = pinManager.isPinOk(pinNr, true);
        #if 0 // for testing
        USER_PRINT(pinManager.isPinAnalog(pinNr) ? "A" : " ");
        USER_PRINT(pinManager.isPinADC1(pinNr) ? "1" : " ");
        USER_PRINT(pinManager.isPinADC2(pinNr) ? "2" : " ");
        USER_PRINT(pinManager.isPinTouch(pinNr) ? "T" : " ");
        USER_PRINT(pinManager.isPinPWM(pinNr) ? " P" : "  ");
        USER_PRINT(pinManager.isPinINT(pinNr) ? "I " : "  ");
        #endif
        USER_PRINTF("%s  %2d\t  %-17s %s\t  %s\n",
          (is_inOut ? "i/o" : "in "),
          pinNr,
          pinManager.getPinOwnerText(pinNr).c_str(),
          pinManager.getPinConflicts(pinNr).c_str(),
          pinManager.getPinSpecialText(pinNr).c_str()
        );
        USER_FLUSH();  // avoid lost lines (Serial buffer overflow)
      }
    }

    #if defined(CONFIG_IDF_TARGET_ESP32P4)
    scanI2C_IDF(global_i2c_bus_handle);  // use IDF v5 API on P4
    #elif !defined(CONFIG_IDF_TARGET_ESP32C5)
    scanI2C(Wire);
    #endif

    strip.createLedmapBinaryCache();

#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(WLEDMM_DISPLAY_MODE)
  {
      int selected = (int)WLEDMM_DISPLAY_MODE;
      Serial.println("\n=== HDMI Mode Selection (30s timeout) ===");
      for (int i = 0; i < (int)HDMI_MODE_COUNT; i++)
        Serial.printf("  %2d: %s%s\n", i, hdmi_mode_names[i], (i == selected) ? "  <-- default" : "");
      Serial.printf("Enter mode number [0-%d] or wait for default: ", (int)HDMI_MODE_COUNT - 1);

      char buf[4] = {};
      uint8_t len = 0;
      uint32_t deadline = millis() + 30000;
      while (millis() < deadline) {
        if (Serial.available()) {
          char c = Serial.read();
          if (c == '\n' || c == '\r') { if (len > 0) break; }
          else if (c >= '0' && c <= '9' && len < 2) { buf[len++] = c; deadline = millis() + 2000; }
        }
        vTaskDelay(1);
      }
      if (len > 0) {
        int choice = atoi(buf);
        if (choice >= 0 && choice < (int)HDMI_MODE_COUNT) selected = choice;
      }
      Serial.printf("\nUsing mode %d: %s\n", selected, hdmi_mode_names[selected]);
      hdmi_display_init(selected);

    // DPI_PANEL_PLACEHOLDER_REMOVE_START
    esp_lcd_dpi_panel_config_t dpi_config_UNUSED = {};
    dpi_config_UNUSED.dpi_clk_src               = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_config.dpi_clock_freq_mhz             = timing.dpi_clock_freq_mhz;
    dpi_config.virtual_channel                = 0;
    dpi_config.in_color_format                = LCD_COLOR_FMT_RGB888;
    dpi_config.out_color_format               = LCD_COLOR_FMT_RGB888;  // must match in_color_format or driver enables color conversion
    dpi_config.num_fbs                        = 2;
    dpi_config.video_timing.h_size            = timing.width;
    dpi_config.video_timing.v_size            = timing.height;
    dpi_config.video_timing.hsync_front_porch = timing.hsync_front_porch;
    dpi_config.video_timing.hsync_pulse_width = timing.hsync_pulse_width;
    dpi_config.video_timing.hsync_back_porch  = timing.hsync_back_porch;
    dpi_config.video_timing.vsync_front_porch = timing.vsync_front_porch;
    dpi_config.video_timing.vsync_pulse_width = timing.vsync_pulse_width;
    dpi_config.video_timing.vsync_back_porch  = timing.vsync_back_porch;
    dpi_config.flags.use_dma2d                = true;
    dpi_config.flags.disable_lp               = true;

    // Three I2C handles for LT8912B register access (main, CEC-DSI, AVI)
    // io_cfg fields must be in declaration order (dev_addr first, scl_speed_hz last)
    esp_lcd_panel_io_handle_t io_main = NULL, io_cec = NULL, io_avi = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg_main = {};
    io_cfg_main.dev_addr             = LT8912B_IO_I2C_MAIN_ADDRESS;
    io_cfg_main.control_phase_bytes  = 1;
    io_cfg_main.dc_bit_offset        = 0;
    io_cfg_main.lcd_cmd_bits         = 8;
    io_cfg_main.lcd_param_bits       = 8;
    io_cfg_main.flags.disable_control_phase = 1;
    io_cfg_main.scl_speed_hz         = 400000;
    esp_lcd_panel_io_i2c_config_t io_cfg_cec = io_cfg_main;
    io_cfg_cec.dev_addr = LT8912B_IO_I2C_CEC_ADDRESS;
    esp_lcd_panel_io_i2c_config_t io_cfg_avi = io_cfg_main;
    io_cfg_avi.dev_addr = LT8912B_IO_I2C_AVI_ADDRESS;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(global_i2c_bus_handle, &io_cfg_main, &io_main));
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(global_i2c_bus_handle, &io_cfg_cec,  &io_cec));
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(global_i2c_bus_handle, &io_cfg_avi,  &io_avi));
    lt8912b_io_main = io_main;  // save for ongoing diagnostics

    // LT8912B vendor config: 720p timing + DSI bus
    lt8912b_vendor_config_t vendor_config = {
      .video_timing = ESP_LCD_LT8912B_VIDEO_TIMING_1280x720_60Hz(),
      .mipi_config = {
        .dsi_bus    = mipi_dsi_bus,
        .dpi_config = &dpi_config,
        .lane_num   = 2,
      },
    };

    vendor_config.video_timing.hfp = timing.hsync_front_porch;
    vendor_config.video_timing.hs = timing.hsync_pulse_width;
    vendor_config.video_timing.hbp = timing.hsync_back_porch;
    vendor_config.video_timing.vfp = timing.vsync_front_porch;
    vendor_config.video_timing.vs = timing.vsync_pulse_width;
    vendor_config.video_timing.vbp = timing.vsync_back_porch;

    vendor_config.video_timing.hact = timing.width;
    vendor_config.video_timing.htotal = timing.hsync_total;
    vendor_config.video_timing.vact = timing.height;
    vendor_config.video_timing.vtotal = timing.vsync_total;

    vendor_config.video_timing.h_polarity = 1;
    vendor_config.video_timing.v_polarity = 0;
    vendor_config.video_timing.vic = timing.vic;
    vendor_config.video_timing.aspect_ratio = timing.aspect_ratio;
    vendor_config.video_timing.pclk_mhz = timing.dpi_clock_freq_mhz;

    // VIC	Format
    //   0	No standard format(custom / undefined)
    //   4	1280×720 @ 60Hz
    //   19	1280×720 @ 50Hz
    //   62	1280×720 @ 30Hz
    //   61	1280×720 @ 25Hz
    //   60	1280×720 @ 24Hz
  
    const esp_lcd_panel_dev_config_t panel_config = {
      .reset_gpio_num = -1,
      .rgb_ele_order = (lcd_rgb_element_order_t)LCD_RGB_ELEMENT_ORDER_RGB,
      .bits_per_pixel = WLEDMM_DISPLAY_DEPTH,
      .vendor_config  = &vendor_config,
    };
    esp_lcd_panel_lt8912b_io_t lt8912b_io = {
      .main    = io_main,
      .cec_dsi = io_cec,
      .avi     = io_avi,
    };

    USER_PRINTLN("Installing LT8912B HDMI bridge driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_lt8912b(&lt8912b_io, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    // Note: esp_lcd_panel_disp_on_off() is not supported by the LT8912B driver (returns ESP_ERR_NOT_SUPPORTED).
    USER_PRINTF("HDMI bridge init: %dx%d @ %dHz (mode %d: %s)\n",
      timing.width, timing.height, timing.fps, selected, hdmi_mode_names[selected]);
    {
      // Register-level diagnostics from Linux DRM driver (lt8912.c):
      //   0xC1 bit 7 = HPD (Hot Plug Detect) — cable physically connected
      //   0xB2 bit 0 = HDMI(1) vs DVI(0) mode — set by driver based on monitor EDID
      uint8_t r_c1 = lt8912b_read_reg(io_main, 0xC1);
      uint8_t r_b2 = lt8912b_read_reg(io_main, 0xB2);
      USER_PRINTF("LT8912B regs: 0xC1=0x%02x (HPD=%d) 0xB2=0x%02x (%s)\n",
        r_c1, (r_c1 >> 7) & 1,
        r_b2, (r_b2 & 1) ? "HDMI" : "DVI");
    }

    // Poll LT8912B for HDMI link — monitor must be connected and locked before continuing
    {
      const uint32_t timeout_ms = 5000;
      const uint32_t poll_ms   = 200;
      uint32_t elapsed = 0;
      bool ready = false;
      USER_PRINT("Waiting for HDMI link");
      while (elapsed < timeout_ms) {
        ready = esp_lcd_panel_lt8912b_is_ready((esp_lcd_panel_t*)panel_handle);
        if (ready) break;
        USER_PRINT(".");
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
        elapsed += poll_ms;
      }
      if (ready) {
        USER_PRINTF(" OK (%ums)\n", elapsed);
      } else {
        USER_PRINTLN(" TIMEOUT — no monitor detected, continuing anyway");
      }
    }

    // Get the DPI panel's native framebuffer
    void* fb0_ptr = NULL;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel_handle, 1, &fb0_ptr));
    display_framebuffer = (uint8_t*)fb0_ptr;
    if (!display_framebuffer) {
      USER_PRINTLN("FATAL: Failed to get display framebuffer!");
      while (1) vTaskDelay(1);
    }
    USER_PRINTF("Display framebuffer at: %p (DPI-internal)\n", display_framebuffer);

    // Allocate a cache-aligned PPA output buffer in PSRAM.
    // PPA on ESP32-P4 checks alignment via esp_cache_get_alignment(MALLOC_CAP_SPIRAM).
    // Use 4096-byte alignment to satisfy both cache and DMA burst requirements.
    const size_t ppa_align = 4096;
    const size_t bytes_per_pixel = WLEDMM_DISPLAY_DEPTH / 8;
    const size_t fb_aligned_size = ((size_t)timing.width * timing.height * bytes_per_pixel + ppa_align - 1) & ~(ppa_align - 1);
    ppa_framebuffer = (uint8_t*)heap_caps_aligned_alloc(ppa_align, fb_aligned_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ppa_framebuffer) {
      USER_PRINTLN("FATAL: Failed to allocate PPA framebuffer!");
      while (1) vTaskDelay(1);
    }
    memset(ppa_framebuffer, 0, fb_aligned_size);
    USER_PRINTF("PPA framebuffer: %p (%u B) addr%%64=%u addr%%256=%u addr%%4096=%u size%%64=%u\n",
      ppa_framebuffer, (unsigned)fb_aligned_size,
      (unsigned)((uintptr_t)ppa_framebuffer % 64),
      (unsigned)((uintptr_t)ppa_framebuffer % 256),
      (unsigned)((uintptr_t)ppa_framebuffer % 4096),
      (unsigned)(fb_aligned_size % 64));

    // Paint a startup test pattern: RGB bands, static colours (beatsin8 returns 0 at millis=0)
    for (uint16_t y = 0; y < timing.height; y++) {
      uint8_t band = (y * 6) / timing.height;  // 0..5 → 6 colour bands across height
      uint8_t r = (band == 0 || band == 3 || band == 4) ? 255 : 0;
      uint8_t g = (band == 1 || band == 3 || band == 5) ? 255 : 0;
      uint8_t b = (band == 2 || band == 4 || band == 5) ? 255 : 0;
      for (uint16_t x = 0; x < timing.width; x++) {
        uint32_t off = ((uint32_t)y * timing.width + x) * bytes_per_pixel;
        display_framebuffer[off + 0] = b;  // DPI framebuffer: BGR order in memory (B at byte 0, R at byte 2)
        display_framebuffer[off + 1] = g;
        display_framebuffer[off + 2] = r;
      }
    }


    // Allocate LGFX sprites for UI overlays (PSRAM-backed)
    myFramebuffer.setPsram(true);
    buttonFramebuffer.setPsram(true);
    myFramebuffer.setColorDepth(WLEDMM_DISPLAY_DEPTH);
    buttonFramebuffer.setColorDepth(WLEDMM_DISPLAY_DEPTH);

    if (!myFramebuffer.createSprite(timing.width, 100)) {
      USER_PRINTLN("Failed to allocate myFramebuffer sprite!");
      while (1) vTaskDelay(1);
    }

    // Calculate buttonFramebuffer height based on button grid layout
    const int cols    = WLEDMM_DISPLAY_BUTTONS_COLS;
    const int padding = 5;
    int rows          = ((int)WLEDMM_DISPLAY_BUTTONS + cols - 1) / cols;
    if (rows < 1) rows = 1;
    int rectWidth = (timing.width - (cols + 1) * padding) / cols;
    int rectHeight = (int)((float)rectWidth * 8.0f / (float)cols);
    int btnFbH     = rows * rectHeight + (rows + 1) * padding;

    if (!buttonFramebuffer.createSprite(timing.width, btnFbH)) {
      USER_PRINTLN("Failed to allocate buttonFramebuffer sprite!");
      while (1) vTaskDelay(1);
    }
    USER_PRINTF("Sprites allocated: main=%dx100, buttons=%dx%d\n", timing.width, timing.width, btnFbH);
    } // end runtime mode selection scope
  }
#endif // CONFIG_IDF_TARGET_ESP32P4 && WLEDMM_DISPLAY_MODE

    xSemaphoreGive(busMutex);

    xTaskCreatePinnedToCore(
    background_loop_blocking,  // Task function
    "BG_Blocking",    // Name
    6244,             // Stack size in words (was 24000)
    NULL,             // Parameters
    1,                // Priority
    NULL,             // Task handle (optional)
    0                 // Core ID (0 or 1)
    );

    xTaskCreatePinnedToCore(
    background_loop_nonblocking,  // Task function
    "Background",     // Name
    10000,             // Stack size in words (was 24000)
    NULL,             // Parameters
    1,                // Priority
    NULL,             // Task handle (optional)
    0                 // Core ID (0 or 1)
    );

    #if defined(ENABLE_VL53L8CX)
    pinMode(TOF_INT_PIN, INPUT_PULLUP);
    sensor_vl53l8cx_top.begin();
    sensor_vl53l8cx_top.init();
    sensor_vl53l8cx_top.set_resolution(vl53l8cx_res);
    sensor_vl53l8cx_top.set_detection_thresholds_enable(0);
    sensor_vl53l8cx_top.set_ranging_mode(VL53L8CX_RANGING_MODE_AUTONOMOUS);
    sensor_vl53l8cx_top.set_external_sync_pin_enable(1);
    sensor_vl53l8cx_top.calibrate_xtalk(3, 1, 1000); // this "fails" but does something important anyway. 
    sensor_vl53l8cx_top.set_ranging_frequency_hz(30);
    sensor_vl53l8cx_top.start_ranging();
    attachInterrupt(digitalPinToInterrupt(TOF_INT_PIN), tofISR, FALLING);

    vl53l8cxMutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(
      sensorTask,     // Function
      "VL53l8CXTask", // Name
      8000,           // Stack size
      NULL,           // Params
      1,              // Priority (Low)
      NULL,           // Handle
      0               // Core 0 (App Core) or 1
    );
    #endif

    USER_PRINT(F("Free heap ")); USER_PRINTLN(ESP.getFreeHeap());USER_PRINTLN();
    USER_PRINTLN(F("WLED initialization done.\n"));

    serial_drain();
    Serial.flush();
    delay(50);
    
    // WLEDMM end
} // endsetup

void WLED::beginStrip() {
  // Initialize NeoPixel Strip and button
  strip.fill(BLACK);    // WLEDMM avoids random colors at power-on
  strip.finalizeInit(); // busses created during deserializeConfig()
  strip.makeAutoSegments();
  strip.setBrightness(0, true); // WLEDMM directly apply BLACK (no transition time)
  strip.setShowCallback(handleOverlayDraw);

  if (turnOnAtBoot) {
    if (briS > 0) bri = briS;
    else if (bri == 0) bri = 128;
  } else {
    // fix for #3196
    briLast = briS; bri = 0;
    strip.fill(BLACK);
    strip.show();
  }
  if (bootPreset > 0) {
    applyPreset(bootPreset, CALL_MODE_INIT);
  }
  colorUpdated(CALL_MODE_INIT);

  // init relay pin
  if (rlyPin>=0) {
    // if (strip.isUpdating()) delay(FRAMETIME_FIXED); // WLEDMM ensure that no background led communication is happening while powering on the strip
    digitalWrite(rlyPin, (rlyMde ? bri : !bri));
    delay(75); // wait for relay to switch and power to stabilize
    strip.show(); // update LEDs
    delay(5);
  }
}

void WLED::initAP(bool resetAP) {
  
  #if defined(WLED_USE_ETHERNET_ONLY)
  return; // we can't start the AP in Ethernet-only mode as there's no WIFi
  #endif

  if (apBehavior == AP_BEHAVIOR_BUTTON_ONLY && !resetAP)
    return;

  if (resetAP) {
    WLED_SET_AP_SSID();
    strcpy_P(apPass, PSTR(WLED_AP_PASS));
  }
  USER_PRINT(F("Opening access point "));
  USER_PRINTLN(apSSID);

  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_stop());

  // Check if we have valid STA credentials
  bool hasValidSTA = strlen(clientSSID) > 0 &&
    strcmp(clientSSID, "Your_Network") != 0 &&
    strcmp(clientSSID, DEFAULT_CLIENT_SSID) != 0;

  if (hasValidSTA) {
    // APSTA mode - run both AP and try to connect to configured network
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_APSTA));
    vTaskDelay(pdMS_TO_TICKS(100));
    USER_PRINTLN("Checking WiFi Stuff");

    wifi_band_mode_t band_mode;
    esp_wifi_get_band_mode(&band_mode);
    USER_PRINTF("Band mode: %s\n", wifi_band_mode_to_string(band_mode));

    wifi_protocols_t protocols;
    esp_wifi_get_protocols(WIFI_IF_STA, &protocols);
    print_wifi_protocols("2.4GHz protocols before set:", protocols.ghz_2g);
    if (band_mode != WIFI_BAND_MODE_2G_ONLY) print_wifi_protocols("5GHz protocols before set:", protocols.ghz_5g);

    wifi_country_t country_check;
    esp_wifi_get_country(&country_check);
    USER_PRINTF("Country: %.2s, channels %d-%d\n", country_check.cc, country_check.schan, country_check.schan + country_check.nchan - 1);

    wifi_protocols_t xprotocols;
    wifi_bandwidths_t bw_config;

    if (band_mode != WIFI_BAND_MODE_2G_ONLY) {
      xprotocols = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N
      };
      ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_protocols(WIFI_IF_STA, &xprotocols));
      bw_config = {
        .ghz_2g = WIFI_BW_HT40,
        .ghz_5g = WIFI_BW_HT40,
      };
      esp_err_t err = esp_wifi_set_bandwidths(WIFI_IF_STA, &bw_config);
      USER_PRINTF("Set bandwidths result: %d (%s)\n", err, esp_err_to_name(err));
    } else {
      ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
      esp_err_t err = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40);
      USER_PRINTF("Set bandwidth result: %d (%s)\n", err, esp_err_to_name(err));
    }

    esp_wifi_get_protocols(WIFI_IF_STA, &xprotocols);

    print_wifi_protocols("2.4GHz protocols after set:", xprotocols.ghz_2g);
    if (band_mode != WIFI_BAND_MODE_2G_ONLY) print_wifi_protocols("5GHz protocols after set:", xprotocols.ghz_5g);

    wifi_config_t wifi_sta_config = {};
    strncpy(reinterpret_cast<char*>(wifi_sta_config.sta.ssid), clientSSID, sizeof(wifi_sta_config.sta.ssid));
    strncpy(reinterpret_cast<char*>(wifi_sta_config.sta.password), clientPass, sizeof(wifi_sta_config.sta.password));
    wifi_sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_sta_config.sta.failure_retry_cnt = 5;
    wifi_sta_config.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    #ifdef WLED_REQUIRE_WIFI_WPA3
    wifi_sta_config.sta.pmf_cfg.capable = true;
    wifi_sta_config.sta.pmf_cfg.required = true;
    wifi_sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    #endif
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
  } else {
    // AP-only mode - no valid STA credentials
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_AP));
    USER_PRINTLN(F("No valid STA credentials, AP-only mode"));
  }

  wifi_config_t wifi_ap_config = {};
  strncpy(reinterpret_cast<char*>(wifi_ap_config.ap.ssid), apSSID, sizeof(wifi_ap_config.ap.ssid));
  strncpy(reinterpret_cast<char*>(wifi_ap_config.ap.password), apPass, sizeof(wifi_ap_config.ap.password));
  wifi_ap_config.ap.ssid_len = strlen(apSSID);
  wifi_ap_config.ap.channel = apChannel;
  wifi_ap_config.ap.max_connection = 100;
  wifi_ap_config.ap.authmode = WIFI_AUTH_WPA_PSK;
  wifi_ap_config.ap.pmf_cfg.required = false;

  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));

  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());

  if (!apActive) // start captive portal if AP active
  {
    DEBUG_PRINTLN(F("Init AP interfaces"));
    server.begin();
    if (udpPort > 0 && udpPort != ntpLocalPort) {
      udpConnected = notifierUdp.begin(udpPort);
    }
    if (udpPort2 > 0 && udpPort2 != ntpLocalPort && udpPort2 != udpPort && udpPort2 != udpRgbPort) {
      udp2Connected = notifier2Udp.begin(udpPort2);
    }
    if (e131Port == ARTNET_DEFAULT_PORT) {
      artnet.stop();
      artnet.begin(e131Universe, ARTNET_PRIORITY);
    } else {
      artnet.stop();
      e131.begin(false, e131Port, e131Universe, E131_MAX_UNIVERSE_COUNT);
      ddp.begin(false, DDP_DEFAULT_PORT);
      if (udpRgbPort > 0 && udpRgbPort != ntpLocalPort && udpRgbPort != udpPort) {
        udpRgbConnected = rgbUdp.begin(udpRgbPort);
      }
    }
    IPAddress apIP = Network.softAPIP();
    USER_PRINT(F("AP IP: "));
    USER_PRINTLN(apIP);
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", Network.softAPIP());
  }
  apActive = true;
}

void WLED::initConnection() {
  USER_PRINTLN("initConnection");

  unsigned long t_wait = millis();
  // while(strip.isUpdating() && (millis() - t_wait < 86)) delay(1); // WLEDMM try to catch a moment when strip is idle
  //if (strip.isUpdating()) USER_PRINTLN("WLED::initConnection: strip still updating.");

#ifdef WLED_ENABLE_WEBSOCKETS
  ws.onEvent(wsEvent);
#endif

  if (staticIP[0] != 0 && staticGateway[0] != 0) {
    // WiFi.config(staticIP, staticGateway, staticSubnet, IPAddress(1, 1, 1, 1));
  } else {
    // WiFi.config(IPAddress((uint32_t)0), IPAddress((uint32_t)0), IPAddress((uint32_t)0));
  }

  lastReconnectAttempt = millis();

#ifdef TROYHACKS_FAILSAFE_BUSSES
  busses.removeAll(); // TROYHACKS FAILSAFE IN CASE BUSSES ARE CAUSING CRASHES
#endif

  // convert the "serverDescription" into a valid DNS hostname (alphanumeric)
  char hostname[25];
  prepareHostname(hostname);
  
#ifndef WLED_USE_ETHERNET_ONLY 
  if (!WLED_WIFI_CONFIGURED) {
    USER_PRINTLN(F("No WiFi connection configured."));  // WLEDMM
    if (!apActive) initAP();        // instantly go to ap mode
    showWelcomePage = true;
    return;
  } else if (!apActive) {
    if (apBehavior == AP_BEHAVIOR_ALWAYS) {
      DEBUG_PRINTLN(F("Access point ALWAYS enabled."));
      initAP();
      showWelcomePage = false;
    }
  }
  
#endif

#ifndef WLED_USE_ETHERNET_ONLY
  if (WLED_WIFI_CONFIGURED && apBehavior != AP_BEHAVIOR_ALWAYS) {
    wifi_config_t wifi_sta_config = {};
    strncpy(reinterpret_cast<char*>(wifi_sta_config.sta.ssid), clientSSID, sizeof(wifi_sta_config.sta.ssid));
    strncpy(reinterpret_cast<char*>(wifi_sta_config.sta.password), clientPass, sizeof(wifi_sta_config.sta.password));
    wifi_sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_sta_config.sta.failure_retry_cnt = 5;
    wifi_sta_config.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    #ifdef WLED_REQUIRE_WIFI_WPA3
    wifi_sta_config.sta.pmf_cfg.capable = true;
    wifi_sta_config.sta.pmf_cfg.required = true;
    wifi_sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    #endif
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
    USER_PRINTF("Connecting to WiFi Station: \"%s\" with password \"%s\"\n", wifi_sta_config.sta.ssid, "********");
  }

#endif

#ifdef WLED_USE_ETHERNET
  USER_PRINTLN(F("Connecting to Ethernet"));
  USER_PRINTF("Network.isConnected = %d\n", Network.isConnected());
  USER_PRINTF("Network.isEthernet = %d\n", Network.isEthernet());
#endif

  // esp_wifi_set_max_tx_power(84);

  Network.setHostname(hostname);
  USER_PRINTLN("end initConnection");
}

void WLED::initInterfaces()
{
  DEBUG_PRINTLN(F("Init STA interfaces"));

// aOtaEnabled=false; strcpy(cmDNS, ""); // WLEDMM use this to disable OTA and mDNS

#ifndef WLED_DISABLE_OTA
  if (aOtaEnabled)
    ArduinoOTA.begin();
#endif

  #ifndef WLED_DISABLE_OTA   // WLEDMM
  if (aOtaEnabled) {
    USER_PRINT(F("           ArduinoOTA: "));
    USER_PRINTLN(ArduinoOTA.getHostname());
  }
  #endif                     // WLEDMM end

  // Set up mDNS responder:
  if (strlen(cmDNS) > 0) {
    // "end" must be called before "begin" is called a 2nd time
    // see https://github.com/esp8266/Arduino/issues/7213
    MDNS.end();
    escapedMac = Network.getEscapedMac();
    sprintf_P(cmDNS, PSTR("wled-%*s"), 6, escapedMac.c_str() + 6);
    MDNS.begin(cmDNS);
    USER_PRINTF("mDNS started: http://%s.local\n", cmDNS); // WLEDMM
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("wled", "tcp", 80);
    MDNS.addServiceTxt("wled", "tcp", "mac", escapedMac.c_str());
  }
  server.begin();

  if (udpPort > 0 && udpPort != ntpLocalPort) {
    udpConnected = false;
    udpConnected = notifierUdp.begin(udpPort);
    if (udpConnected && udpPort2 != udpPort && udpPort2 != udpRgbPort) udp2Connected = notifier2Udp.begin(udpPort2);
  }
  if (ntpEnabled) {
    ntpConnected = ntpUdp.begin(ntpLocalPort);
  }
  if (e131Port == ARTNET_DEFAULT_PORT) {
    artnet.stop();
    artnet_listening = artnet.begin(e131Universe, ARTNET_PRIORITY);
  } else {
    artnet.stop();
    e131_listening = e131.begin(false, e131Port, e131Universe, E131_MAX_UNIVERSE_COUNT);
    ddp_listening = ddp.begin(false, DDP_DEFAULT_PORT);
    if (udpConnected && udpRgbPort != udpPort) {
      udpRgbConnected = rgbUdp.begin(udpRgbPort);
    }
  }
  vTaskDelay(pdMS_TO_TICKS(500));
  interfacesInited = true;
  wasConnected = true;
}

void WLED::handleConnection() {
  static byte stacO = 0;
  static uint32_t lastHeap = UINT32_MAX;
  static unsigned long heapTime = 0;
  static bool wifiStartAttempted = false;      // NEW: Track if we've tried to start WiFi
  static unsigned long lastWifiRetry = 0;       // NEW: For periodic WiFi retry
  unsigned long now = millis();

  if (now < 2000 && (!WLED_WIFI_CONFIGURED || apBehavior == AP_BEHAVIOR_ALWAYS))
    return;

  // === NEW: Ensure WiFi is started regardless of Ethernet state ===
  #if !defined(WLED_USE_ETHERNET_ONLY)
  if (!wifiStartAttempted && WLED_WIFI_CONFIGURED && apBehavior != AP_BEHAVIOR_ALWAYS) {
    USER_PRINTLN(F("Starting WiFi STA mode..."));

    wifi_config_t wifi_sta_config = {};
    strncpy(reinterpret_cast<char*>(wifi_sta_config.sta.ssid), clientSSID, sizeof(wifi_sta_config.sta.ssid));
    strncpy(reinterpret_cast<char*>(wifi_sta_config.sta.password), clientPass, sizeof(wifi_sta_config.sta.password));
    wifi_sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_sta_config.sta.failure_retry_cnt = 5;
    wifi_sta_config.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    #ifdef WLED_REQUIRE_WIFI_WPA3
    wifi_sta_config.sta.pmf_cfg.capable = true;
    wifi_sta_config.sta.pmf_cfg.required = true;
    wifi_sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    #endif

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));

    // Only set mode if not already in APSTA mode
    wifi_mode_t currentMode;
    esp_wifi_get_mode(&currentMode);
    if (currentMode != WIFI_MODE_APSTA) {
      ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());

    wifiStartAttempted = true;
    lastWifiRetry = now;
    lastReconnectAttempt = now;
    USER_PRINTF("WiFi connecting to: \"%s\"\n", clientSSID);
  }
  #endif
  // === END NEW SECTION ===

  if (lastReconnectAttempt == 0) {
    DEBUG_PRINTLN(F("lastReconnectAttempt == 0"));
    initConnection();
    wifiStartAttempted = true;  // initConnection also starts WiFi
    return;
  }

  // AP client count handling (unchanged)
  byte stac = g_ap_client_count;

  if (stac != stacO) {
    stacO = stac;
    USER_PRINT(F("Connected AP clients: "));
    USER_PRINTLN(stac);
    if (!WLED_CONNECTED && WLED_WIFI_CONFIGURED) {
      if (stac) {
        // WiFi.disconnect();
      } else {
        initConnection();
        wifiStartAttempted = true;
        return;
      }
    }
  }

  if (forceReconnect) {
    USER_PRINTLN(F("Forcing reconnect."));
    initConnection();
    interfacesInited = false;
    forceReconnect = false;
    wasConnected = false;
    wifiStartAttempted = true;
    return;
  }

  // === NEW: Retry WiFi connection periodically if Ethernet is up but WiFi isn't ===
  #if defined(WLED_USE_ETHERNET) && !defined(WLED_USE_ETHERNET_ONLY)
  if (eth_is_connected && !wifi_is_connected && wifiStartAttempted && WLED_WIFI_CONFIGURED) {
    if (now - lastWifiRetry > 30000) {  // Retry every 30 seconds
      USER_PRINTLN(F("Ethernet connected, retrying WiFi..."));
      ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
      lastWifiRetry = now;
    }
  }
  #endif
  // === END NEW SECTION ===

  // Check if ANY network is disconnected that we care about
  // Modified logic: Only consider "disconnected" if ALL networks are down
  bool anyNetworkConnected = eth_is_connected || wifi_is_connected;

  if (!anyNetworkConnected) {
    if (interfacesInited) {
      USER_PRINTLN(F("Disconnected!"));
      interfacesInited = false;
      initConnection();
      wifiStartAttempted = true;
      return;
    }
    // send improv failed 6 seconds after second init attempt (24 sec. after provisioning)
    if (improvActive > 2 && now - lastReconnectAttempt > 6000) {
      sendImprovStateResponse(0x03, true);
      improvActive = 2;
    }
    if (now - lastReconnectAttempt > ((stac) ? 300000 : 18000) && WLED_WIFI_CONFIGURED) {
      if (improvActive == 2) improvActive = 3;
      USER_PRINTLN(F("Last reconnect too old."));
      initConnection();
      wifiStartAttempted = true;
      return;
    }
    #if !defined(WLED_USE_ETHERNET_ONLY)
    uint16_t connect_timeout = 12000;
    #ifdef CONFIG_IDF_TARGET_ESP32C5
    connect_timeout *= 3; // 5ghz can take longer to spin up.
    #endif
    if (!apActive && now - lastReconnectAttempt > connect_timeout && (!wasConnected || apBehavior == AP_BEHAVIOR_NO_CONN)) {
      USER_PRINTLN(F("Not connected, starting AP."));
      initAP();
    }
    #endif
  }

  // Initialize interfaces when we have ANY connection
  if (anyNetworkConnected && !interfacesInited) {
    USER_PRINTLN();
    USER_PRINT(F("Connected!\nIP address: http://"));
    USER_PRINT(Network.localIP());
    IPAddress backup;
    if (Network.isEthernet() && !Network.isWiFi()) {
      USER_PRINT(F(" via Ethernet (Primary Route)\n"));
      backup = Network.getWiFiIP();
      if (backup != INADDR_NONE && backup != IPAddress(IPADDR_BROADCAST)) {
        USER_PRINT(F("IP address: http://"));
        USER_PRINT(backup);
        USER_PRINT(F(" via WiFi (Backup Route)\n"));
      }
    } else if (Network.isWiFi() && !Network.isEthernet()) {
      USER_PRINT(F(" via WiFi (Primary Route)\n"));
      backup = Network.getEthernetIP();
      if (backup != INADDR_NONE && backup != IPAddress(IPADDR_BROADCAST)) {
        USER_PRINT(F("IP address: http://"));
        USER_PRINT(backup);
        USER_PRINT(F(" via Ethernet (Backup Route)\n"));
      }
    } else {
      USER_PRINT(F(" via Unknown (This shouldn't happen!)"));
    }
    USER_PRINTLN();

    if (improvActive) {
      if (improvError == 3) sendImprovStateResponse(0x00, true);
      sendImprovStateResponse(0x04);
      if (improvActive > 1) sendImprovIPRPCResult(ImprovRPCType::Command_Wifi);
    }
    initInterfaces();
    userConnected();
    usermods.connected();
    lastMqttReconnectAttempt = 0;
    ws.onEvent(wsEvent);
  }

  // === NEW: Log when WiFi comes up after Ethernet ===
  #if defined(WLED_USE_ETHERNET)
  static bool wifiWasConnected = false;
  if (wifi_is_connected && !wifiWasConnected) {
    wifiWasConnected = true;
  } else if (!wifi_is_connected && wifiWasConnected) {
    wifiWasConnected = false;
  }
  #endif
  // === END NEW SECTION ===
}

// If status LED pin is allocated for other uses, does nothing
// else blink at 1Hz when WLED_CONNECTED is false (no WiFi, ?? no Ethernet ??)
// else blink at 2Hz when MQTT is enabled but not connected
// else turn the status LED off
void WLED::handleStatusLED()
{
  #if defined(STATUSLED)
  uint32_t c = 0;

  #if STATUSLED>=0
  if (pinManager.isPinAllocated(STATUSLED)) {
    return; //lower priority if something else uses the same pin
  }
  #endif

  if (WLED_CONNECTED) {
    c = RGBW32(0,255,0,0);
    ledStatusType = 2;
  } else if (WLED_MQTT_CONNECTED) {
    c = RGBW32(0,128,0,0);
    ledStatusType = 4;
  } else if (apActive) {
    c = RGBW32(0,0,255,0);
    ledStatusType = 1;
  }
  if (ledStatusType) {
    if (millis() - ledStatusLastMillis >= (1000/ledStatusType)) {
      ledStatusLastMillis = millis();
#if 0
      // WLEDMM un-comment this to stop the blinking
      if ((ledStatusType != 2) && (ledStatusType != 4))
        ledStatusState = !ledStatusState;
      else
        ledStatusState = HIGH;
#else
        ledStatusState = !ledStatusState;
#endif
      #if STATUSLED>=0
      digitalWrite(STATUSLED, ledStatusState);
      #else
      busses.setStatusPixel(ledStatusState ? c : 0);
      #endif
    }
  } else {
    #if STATUSLED>=0
      #ifdef STATUSLEDINVERTED
      digitalWrite(STATUSLED, HIGH);
      #else
      digitalWrite(STATUSLED, LOW);
      #endif
    #else
      busses.setStatusPixel(0);
    #endif
  }
  #endif
}

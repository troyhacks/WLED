// HDMI display subsystem — Olimex ESP32-P4-PC via LT8912B bridge (LT8912B chip).
// All HDMI-specific code lives here; wled.cpp calls the public functions
// declared in wled_hdmi.h: hdmi_setup(), hdmi_blit(), hdmi_print_mode_menu(), hdmi_switch_mode().

#include "wled.h"

#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include "esp_ldo_regulator.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_lt8912b.h"
#endif // CONFIG_IDF_TARGET_ESP32P4

#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(WLEDMM_DISPLAY_MODE)

// ============================================================
// Aspect ratio constants for HDMI AVI InfoFrame PB2
// ============================================================
#define HDMI_AR_NONE  0x00  // No data / no standard mapping (e.g. 16:10)
#define HDMI_AR_4_3   0x01
#define HDMI_AR_16_9  0x02

// ============================================================
// X-macro mode table — empirically confirmed working modes only.
// Columns: name, vic, fps, width, height, pclk_khz, hfp, hsw, hbp, vfp, vsw, vbp, aspect_ratio
//
// ESP32-P4 PHY/DPI clock constraints:
//   DPI source = PLL_F240M (240 MHz) → pixel clock = 240 / integer divider
//   PHY source = PLL_F20M  ( 20 MHz) → lane rate   = 20  × integer multiplier
//   For 2 lanes 24bpp: pixel_clock = lane_rate / 12
//   Valid pixel clocks (exact ratio 1.5, integer divisors of 240 AND multiples of 5 MHz):
//     20, 30, 40, 60, 80 MHz.  Any non-multiple-of-5 snaps badly — avoid.
//
//   Effective DMA load = (active_pixels / total_pixels) * pclk * 3 bytes.
//   Observed limits:
//     30 MHz: ≤~62 MB/s usable  (720x576@50 works  [62 MB/s]; 640x480@75 fails [69 MB/s])
//     40 MHz: ≤~88 MB/s usable  (800x600@60 works  [87 MB/s]; 640x480@100 crashes [92 MB/s])
//     60 MHz: ≤~132 MB/s usable (1024x768@60 works [132 MB/s]; 1280x1024@35 fails [138 MB/s])
//     80 MHz: DMA underruns observed even at 1920x1080@30 — no confirmed working mode yet.
//
// disable_lp is hardcoded true (HS-only) — LP mode universally breaks LT8912B output.
// All modes use PLL_F240M (MIPI_DSI_DPI_CLK_SRC_DEFAULT). PLL_F160M (53.33 MHz) causes
// diagonal skewing because the HAL stores dpi_clock_freq_mhz as integer (53, not 53.333),
// making dpi2lane_clk_ratio = 640/53/8 = 1.509 instead of exactly 1.5.
// fps column is nominal/rounded; actual = pclk_khz*1000 / (Htot*Vtot).
// ============================================================
#define HDMI_MODE_LIST \
  /* ===== 30 MHz — DPI=240/8, PHY=360Mbps (M=18 N=1) ===== */ \
  X(P4_720x576_50HZ,       0, 50,  720, 576, 30000,  12,  64, 164,  5,  5, 39, HDMI_AR_4_3 ) /* Htot=960  Vtot=625  50.0Hz PAL  DMA=62MB/s */ \
  \
  /* ===== 40 MHz — DPI=240/6, PHY=480Mbps (N=24) ===== */ \
  X(P4_800x600_60HZ,       0, 60,  800, 600, 40000,  40, 128,  88,  1,  4, 23, HDMI_AR_4_3 ) /* Htot=1056 Vtot=628  60.3Hz       DMA=87MB/s */ \
  X(P4_1024x576_57HZ,      0, 57, 1024, 576, 40000,   8,  48,  40,  3,  5, 42, HDMI_AR_16_9) /* Htot=1120 Vtot=626  57.1Hz       DMA=84MB/s */ \
  \
  /* ===== 60 MHz — DPI=240/4, PHY=720Mbps (N=36) ===== */ \
  X(P4_1280x720_50HZ,      0, 50, 1280, 720, 60000, 110,  40, 170,  5,  5, 20, HDMI_AR_16_9) /* Htot=1600 Vtot=750  50.00Hz      DMA=115MB/s */ \
  X(P4_1280x720_60HZ,      0, 60, 1280, 720, 60000,  10,  32,  28,  3,  5, 13, HDMI_AR_16_9) /* Htot=1350 Vtot=741  59.98Hz      DMA=123MB/s */ \
  X(P4_1280x800_50HZ,      0, 50, 1280, 800, 60000,  48,  32,  80,  3,  5, 25, HDMI_AR_NONE) /* Htot=1440 Vtot=833  50.0Hz       DMA=128MB/s */ \
  X(P4_1024x768_60HZ,      0, 60, 1024, 768, 60000,  48,  32,  80,  3,  5, 69, HDMI_AR_4_3 ) /* Htot=1184 Vtot=845  59.9Hz       DMA=132MB/s */ \

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
  uint32_t lane_bit_rate_mbps;  // DSI lane bit rate
  uint8_t  aspect_ratio;        // HDMI_AR_NONE/4_3/16_9
  bool     disable_lp;          // false = allow LP during blanking (helps DMA at high pclk); true = HS-only
  mipi_dsi_dpi_clock_source_t dpi_clk_src;  // PLL source for DPI pixel clock
};

static bool hdmi_get_dpi_config(hdmi_mode_t mode, uint8_t dsi_lanes, hdmi_dpi_config_t &out) {
  if (mode >= HDMI_MODE_COUNT) return false;
  const hdmi_cea861_entry_t &t = hdmi_cea861_table[mode];
  out.vic                = t.vic;
  out.fps                = t.fps;
  out.width              = t.width;
  out.height             = t.height;
  out.dpi_clock_freq_mhz = (t.pixel_clock_khz + 500) / 1000;  // round to nearest MHz
  out.hsync_front_porch  = t.hfp;
  out.hsync_pulse_width  = t.hsw;
  out.hsync_back_porch   = t.hbp;
  out.vsync_front_porch  = t.vfp;
  out.vsync_pulse_width  = t.vsw;
  out.vsync_back_porch   = t.vbp;
  out.hsync_total        = t.width + t.hfp + t.hsw + t.hbp;
  out.vsync_total        = t.height + t.vfp + t.vsw + t.vbp;
  out.lane_bit_rate_mbps = (uint32_t)((uint64_t)t.pixel_clock_khz * 24 / dsi_lanes / 1000);
  out.aspect_ratio       = t.aspect_ratio;
  out.disable_lp         = true;  // LP mode universally breaks LT8912B output on this hardware
  out.dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT;  // PLL_F240M — only source that gives integer ratio
  return true;
}

// Runtime display dimensions — set at init, replacing compile-time W/H defines
uint16_t wledmm_display_w = 0;
uint16_t wledmm_display_h = 0;
#define WLEDMM_DISPLAY_W   wledmm_display_w
#define WLEDMM_DISPLAY_H   wledmm_display_h
#define WLEDMM_DISPLAY_DEPTH 24  // RGB888 always for HDMI

// ============================================================
// Direct register read from an LT8912B I2C handle.
// Key regs (from Linux DRM driver lt8912.c):
//   MAIN 0xC1 bit 7 : HPD — cable plugged in. NOTE: HPD_CBUS pin defaults to MHL CBUS
//                     mode per datasheet §3.2.2; must be switched to HPD via I2C write.
//   MAIN 0xB2 bit 0 : 1=HDMI mode, 0=DVI. Set from monitor EDID — but LT8912B has no
//                     DDC support (datasheet §2.2), so this likely always reads 0 (DVI).
// ============================================================
static uint8_t lt8912b_read_reg(esp_lcd_panel_io_handle_t io, uint8_t reg) {
  uint8_t val = 0xFF;
  esp_lcd_panel_io_rx_param(io, reg, &val, 1);
  return val;
}

// ============================================================
// Tear down the HDMI display stack (panel → I2C handles → DSI bus).
// NULLs both FB pointers first so the blit loop stops and the swap can't resurrect a stale pointer.
// ============================================================
static void hdmi_display_deinit() {
  display_framebuffer       = NULL;
  display_front_framebuffer = NULL;
  vTaskDelay(pdMS_TO_TICKS(150));       // let any in-progress blit + vsync finish

  // Delete panel first (it may use I2C handles during teardown)
  if (panel_handle)    { esp_lcd_panel_del(panel_handle);           panel_handle    = NULL; }
  vTaskDelay(pdMS_TO_TICKS(50));  // let any in-flight vsync/DMA interrupt drain before freeing the bus

  // Delete I2C handles after panel is gone
  if (lt8912b_io_avi)  { esp_lcd_panel_io_del(lt8912b_io_avi);   lt8912b_io_avi  = NULL; }
  if (lt8912b_io_cec)  { esp_lcd_panel_io_del(lt8912b_io_cec);   lt8912b_io_cec  = NULL; }
  if (lt8912b_io_main) { esp_lcd_panel_io_del(lt8912b_io_main);  lt8912b_io_main = NULL; }

  // Delete DSI bus last (panel depends on it)
  if (lt8912b_dsi_bus) { esp_lcd_del_dsi_bus(lt8912b_dsi_bus);   lt8912b_dsi_bus = NULL; }

  wledmm_display_w = 0;
  wledmm_display_h = 0;
}

// ============================================================
// Low-level init: takes a pre-built timing struct + display label.
// Called by hdmi_display_init().
// ============================================================
static void hdmi_display_init_timing(const hdmi_dpi_config_t& timing, const char* label) {
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

  wledmm_display_w = timing.width;
  wledmm_display_h = timing.height;
  bus_config.lane_bit_rate_mbps = timing.lane_bit_rate_mbps;

  USER_PRINTLN("Initialize MIPI DSI bus");
  ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &lt8912b_dsi_bus));

  esp_lcd_dpi_panel_config_t dpi_config = {};
  dpi_config.dpi_clk_src                    = timing.dpi_clk_src;
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
  dpi_config.flags.disable_lp               = timing.disable_lp;

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
  // Note: esp_lcd_panel_disp_on_off() is not supported by the LT8912B driver (returns ESP_ERR_NOT_SUPPORTED).
  USER_PRINTF("HDMI bridge init: %dx%d pclk=%uMHz lane=%uMbps disable_lp=%d (%s)\n",
    timing.width, timing.height, timing.dpi_clock_freq_mhz, timing.lane_bit_rate_mbps,
    (int)timing.disable_lp, label ? label : "?");
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

  void* fb0_ptr = NULL, *fb1_ptr = NULL;
  ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel_handle, 2, &fb0_ptr, &fb1_ptr));
  if (!fb0_ptr || !fb1_ptr) { USER_PRINTLN("FATAL: no display framebuffers!"); while (1) vTaskDelay(1); }
  const size_t fb_bytes = (size_t)timing.width * timing.height * (WLEDMM_DISPLAY_DEPTH / 8);
  memset(fb0_ptr, 0, fb_bytes);
  memset(fb1_ptr, 0, fb_bytes);
  display_front_framebuffer = (uint8_t*)fb0_ptr;  // DPI starts displaying fb0
  display_framebuffer       = (uint8_t*)fb1_ptr;  // we write into fb1 first
  USER_PRINTF("Framebuffers: front=%p back=%p\n", display_front_framebuffer, display_framebuffer);

}

// ============================================================
// Mode-index wrapper: looks up timing from table then calls init_timing.
// ============================================================
static void hdmi_display_init(int mode) {
  hdmi_dpi_config_t timing = {};
  hdmi_get_dpi_config((hdmi_mode_t)mode, 2, timing);
  hdmi_display_init_timing(timing, hdmi_mode_names[mode]);
  hdmi_current_mode = mode;
}

// ============================================================
// hdmi_setup() — boot-time init. Use 'm' in serial console to switch modes at runtime.
// Call once from WLED::setup().
// ============================================================
void hdmi_setup() {
  int selected = (int)WLEDMM_DISPLAY_MODE;
  Serial.printf("HDMI: starting mode %d: %s (type 'm' to switch)\n", selected, hdmi_mode_names[selected]);
  hdmi_display_init(selected);
}

// ============================================================
// hdmi_print_mode_menu() — print numbered mode list to Serial.
// ============================================================
void hdmi_print_mode_menu() {
  Serial.printf("=== HDMI modes (0-%d) ===\n", (int)HDMI_MODE_COUNT - 1);
  for (int i = 0; i < (int)HDMI_MODE_COUNT; i++)
    Serial.printf("  %d: %s\n", i, hdmi_mode_names[i]);
  Serial.print("Enter number + Enter: ");
}

// ============================================================
// hdmi_switch_mode() — runtime mode switch, called from handleSerial() after menu input.
// ============================================================
void hdmi_switch_mode(int mode) {
  if (mode < 0 || mode >= (int)HDMI_MODE_COUNT) {
    Serial.printf("HDMI: invalid mode %d\n", mode);
    return;
  }
  Serial.printf("HDMI: switching to %d: %s\n", mode, hdmi_mode_names[mode]);
  hdmi_display_deinit();
  hdmi_display_init(mode);
}

#endif // CONFIG_IDF_TARGET_ESP32P4 && WLEDMM_DISPLAY_MODE

// ============================================================
// hdmi_blit() — PPA scale-rotate-mirror: LED bus buffer → display framebuffer.
// Call from WLED::loop() every frame.
// ============================================================
#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(WLEDMM_DISPLAY_MODE) && defined(CONFIG_SOC_PPA_SUPPORTED)
void hdmi_blit() {
  if (!display_framebuffer || !panel_handle) return;

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
          srm_cfg.rgb_swap           = true;
          srm_cfg.byte_swap          = false;
          srm_cfg.mode               = PPA_TRANS_MODE_BLOCKING;

          if (ESP_ERROR_CHECK_WITHOUT_ABORT(ppa_do_scale_rotate_mirror(ppa_srm_handle, &srm_cfg)) == ESP_OK) {
            blit_ok++;
            // With num_fbs=2 the DPI DMA linked list alternates fb0→fb1→fb0→... automatically.
            // Just swap our software pointers — the hardware picks up the new back buffer on the
            // next frame with no explicit draw_bitmap call needed (and draw_bitmap corrupts IDF state).
            uint8_t* tmp          = display_front_framebuffer;
            display_front_framebuffer = display_framebuffer;
            display_framebuffer       = tmp;
          } else {
            blit_fail++;
          }

          // Diagnostic: every 5s — always log if HPD=0 (no monitor), silent when HPD=1
          uint32_t now_ms = millis();
          if (now_ms - blit_log_ms >= 5000) {
            blit_log_ms = now_ms;
            uint8_t r_c1 = lt8912b_io_main ? lt8912b_read_reg(lt8912b_io_main, 0xC1) : 0;
            bool hpd = (r_c1 >> 7) & 1;
            if (!hpd) {
              USER_PRINTF("HDMI: no monitor connected (HPD=0) blit ok=%u fail=%u\n", blit_ok, blit_fail);
            }
            blit_ok = blit_fail = 0;
          }
        }
      }
      xSemaphoreGive(busMutex);
    }
  }

}
#endif // CONFIG_IDF_TARGET_ESP32P4 && WLEDMM_DISPLAY_MODE && CONFIG_SOC_PPA_SUPPORTED

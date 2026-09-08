// HDMI display subsystem — Olimex ESP32-P4-PC via LT8912B bridge (LT8912B chip).
// All HDMI-specific code lives here; wled.cpp calls the public functions
// declared in wled_hdmi.h: hdmi_setup(), hdmi_blit(), hdmi_print_mode_menu(), hdmi_switch_mode().

#include "wled.h"
#include "wled_hdmi.h"

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
  X(P4_720x576_50HZ,      17, 50,  720, 576, 30000,  12,  64, 164,  5,  5, 39, HDMI_AR_4_3 ) /* Htot=960  Vtot=625  50.0Hz PAL  DMA=62MB/s  VIC17=720x576p@50 4:3 */ \
  \
  /* ===== 40 MHz — DPI=240/6, PHY=480Mbps (N=24) ===== */ \
  X(P4_800x600_60HZ,       0, 60,  800, 600, 40000,  40, 128,  88,  1,  4, 23, HDMI_AR_4_3 ) /* Htot=1056 Vtot=628  60.3Hz       DMA=87MB/s  no CEA VIC */ \
  X(P4_1024x576_57HZ,      0, 57, 1024, 576, 40000,   8,  48,  40,  3,  5, 42, HDMI_AR_16_9) /* Htot=1120 Vtot=626  57.1Hz       DMA=84MB/s  no CEA VIC */ \
  \
  /* ===== 60 MHz — DPI=240/4, PHY=720Mbps (N=36) ===== */ \
  X(P4_1280x720_50HZ,     19, 50, 1280, 720, 60000, 110,  40, 170,  5,  5, 20, HDMI_AR_16_9) /* Htot=1600 Vtot=750  50.00Hz      DMA=115MB/s VIC19=1280x720p@50 */ \
  X(P4_1280x720_60HZ,      4, 60, 1280, 720, 60000,  10,  32,  28,  3,  5, 13, HDMI_AR_16_9) /* Htot=1350 Vtot=741  59.98Hz      DMA=123MB/s VIC4=1280x720p@60  */ \
  X(P4_1280x800_50HZ,      0, 50, 1280, 800, 60000,  48,  32,  80,  3,  5, 25, HDMI_AR_NONE) /* Htot=1440 Vtot=833  50.0Hz       DMA=128MB/s no CEA VIC (16:10) */ \
  X(P4_1024x768_60HZ,      0, 60, 1024, 768, 60000,  48,  32,  80,  3,  5, 69, HDMI_AR_4_3 ) /* Htot=1184 Vtot=845  59.9Hz       DMA=132MB/s no CEA VIC */ \

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

// Dedicated IDF v5 I2C master bus for the LT8912B HDMI bridge.
// Created lazily inside hdmi_setup() so we don't depend on whatever the
// rest of WLED does to populate global_i2c_bus_handle.  Stays alive for
// the full app lifetime; torn down by hdmi_display_deinit().
static i2c_master_bus_handle_t hdmi_i2c_bus = NULL;
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

static void lt8912b_write_reg(esp_lcd_panel_io_handle_t io, uint8_t reg, uint8_t val) {
  esp_lcd_panel_io_tx_param(io, reg, &val, 1);
}

// ============================================================
// EDID reading and parsing.
// The LT8912B proxies the connected monitor's EDID at I2C address 0x50.
// Standard EDID block = 128 bytes.  CEA-861 extension = another 128 bytes.
// Call hdmi_edid_init() once after the HDMI link is established.
// ============================================================

hdmi_edid_info_t hdmi_edid_info = {};  // extern declared in wled_hdmi.h

// Read one 128-byte block from the DDC EDID proxy at 0x50.
// offset=0x00 → base block, offset=0x80 → first extension block.
// Uses a temporary IDF dev handle (same pattern as probeI2C_unknown in util.cpp).
static bool edid_read_block(uint8_t offset, uint8_t *buf) {
  if (!hdmi_i2c_bus) return false;
  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address  = 0x50;
  cfg.scl_speed_hz    = 100000;  // DDC spec: 100 kHz (don't use 400 kHz — some monitors are slow)
  i2c_master_dev_handle_t dev = nullptr;
  if (i2c_master_bus_add_device(hdmi_i2c_bus, &cfg, &dev) != ESP_OK) return false;
  bool ok = (i2c_master_transmit_receive(dev, &offset, 1, buf, 128, 100) == ESP_OK);
  if (!ok && offset == 0x00)  // fallback: plain read for base block (no sub-address)
    ok = (i2c_master_receive(dev, buf, 128, 100) == ESP_OK);
  i2c_master_bus_rm_device(dev);
  return ok;
}

// Decode the 5-bit packed 3-letter manufacturer code from EDID bytes 8–9.
static void edid_decode_manufacturer(const uint8_t *e, char *out) {
  uint16_t mid = ((uint16_t)e[8] << 8) | e[9];
  out[0] = 'A' + ((mid >> 10) & 0x1F) - 1;
  out[1] = 'A' + ((mid >>  5) & 0x1F) - 1;
  out[2] = 'A' + ( mid        & 0x1F) - 1;
  out[3] = '\0';
  // Sanity check — replace garbage with '?'
  for (int i = 0; i < 3; i++)
    if (out[i] < 'A' || out[i] > 'Z') out[i] = '?';
}

// Copy the monitor name from an 0xFC descriptor block.
static void edid_decode_name(const uint8_t *d, char *out, size_t out_len) {
  size_t i = 0;
  for (; i < 13 && i + 1 < out_len; i++) {
    char c = (char)d[5 + i];
    if (c == '\n' || c == '\r' || c == '\0') break;
    out[i] = c;
  }
  // Trim trailing spaces
  while (i > 0 && out[i - 1] == ' ') i--;
  out[i] = '\0';
}

// Parse a Detailed Timing Descriptor (DTD).  Returns false if it's a monitor descriptor, not a timing.
static bool edid_parse_dtd(const uint8_t *d, uint16_t *h_act, uint16_t *v_act, uint32_t *pclk_khz) {
  if (d[0] == 0 && d[1] == 0) return false;  // monitor descriptor, not a timing
  *pclk_khz = ((uint32_t)d[1] << 8 | d[0]) * 10;   // 10 kHz units
  *h_act    = (uint16_t)(d[2] | ((uint16_t)(d[4] & 0xF0) << 4));
  *v_act    = (uint16_t)(d[5] | ((uint16_t)(d[7] & 0xF0) << 4));
  return (*pclk_khz > 0 && *h_act > 0 && *v_act > 0);
}

static void hdmi_edid_init() {
  hdmi_edid_info = {};
  if (!hdmi_i2c_bus) return;

  // ── Base block (128 bytes) ────────────────────────────────────────────────
  uint8_t base[128] = {};
  if (!edid_read_block(0x00, base)) {
    USER_PRINTLN("EDID: DDC read failed (no monitor or link not ready)");
    return;
  }

  static const uint8_t edid_hdr[8] = {0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00};
  if (memcmp(base, edid_hdr, 8) != 0) {
    USER_PRINTLN("EDID: invalid header — skipping");
    return;
  }
  uint8_t csum = 0;
  for (int i = 0; i < 128; i++) csum += base[i];
  if (csum != 0) USER_PRINTF("EDID: base block checksum FAIL (sum=0x%02X)\n", csum);

  hdmi_edid_info.present = true;

  // Manufacturer, product, year
  edid_decode_manufacturer(base, hdmi_edid_info.manufacturer);
  hdmi_edid_info.product_code = (uint16_t)base[10] | ((uint16_t)base[11] << 8);
  hdmi_edid_info.year         = (uint16_t)base[17] + 1990;

  // Four 18-byte descriptor blocks at offsets 54, 72, 90, 108
  bool got_preferred = false;
  for (int b = 0; b < 4; b++) {
    const uint8_t *d = base + 54 + b * 18;
    if (d[0] == 0 && d[1] == 0 && d[2] == 0) {
      if (d[3] == 0xFC)  // monitor name
        edid_decode_name(d, hdmi_edid_info.monitor_name, sizeof(hdmi_edid_info.monitor_name));
    } else if (!got_preferred) {
      got_preferred = edid_parse_dtd(d,
          &hdmi_edid_info.preferred_hactive,
          &hdmi_edid_info.preferred_vactive,
          &hdmi_edid_info.preferred_pclk_khz);
    }
  }

  // Standard timings: 8 × 2 bytes at bytes 38–53
  for (int i = 0; i < 8 && hdmi_edid_info.std_timing_count < 8; i++) {
    uint8_t b0 = base[38 + i * 2], b1 = base[39 + i * 2];
    if (b0 == 0x01 && b1 == 0x01) continue;   // unused slot
    uint16_t w  = ((uint16_t)b0 + 31) * 8;
    uint8_t  ar = (b1 >> 6) & 3;
    uint16_t h;
    switch (ar) {
      case 0:  h = (uint16_t)((uint32_t)w * 10 / 16); break;  // 16:10
      case 1:  h = (uint16_t)((uint32_t)w *  3 /  4); break;  // 4:3
      case 2:  h = (uint16_t)((uint32_t)w *  4 /  5); break;  // 5:4
      default: h = (uint16_t)((uint32_t)w *  9 / 16); break;  // 16:9
    }
    uint8_t n = hdmi_edid_info.std_timing_count++;
    hdmi_edid_info.std_hactive[n] = w;
    hdmi_edid_info.std_vactive[n] = h;
  }

  // ── CEA-861 extension block (byte 126 = extension count) ─────────────────
  if (base[126] > 0) {
    uint8_t cea[128] = {};
    if (edid_read_block(0x80, cea) && cea[0] == 0x02) {  // tag 0x02 = CEA-861
      uint8_t csum2 = 0;
      for (int i = 0; i < 128; i++) csum2 += cea[i];
      if (csum2 != 0) {
        USER_PRINTF("EDID: CEA extension checksum FAIL (sum=0x%02X)\n", csum2);
      } else {
        uint8_t dtd_off = cea[2];   // byte offset to first 18-byte DTD within this block
        int pos = 4;                // data blocks begin at byte 4
        while (pos < (int)dtd_off && pos < 126) {
          uint8_t tag = (cea[pos] >> 5) & 0x07;
          uint8_t len =  cea[pos]       & 0x1F;
          if (pos + 1 + len > 128) break;
          if (tag == 2) {           // Video Data Block — SVD list
            for (int v = 1; v <= len && hdmi_edid_info.cea_vic_count < 32; v++) {
              uint8_t vic = cea[pos + v] & 0x7F;
              if (vic) hdmi_edid_info.cea_vic[hdmi_edid_info.cea_vic_count++] = vic;
            }
          } else if (tag == 3 && len >= 3) {  // Vendor Specific Data Block
            // HDMI LLC OUI is 0x000C03, stored little-endian on wire: 03 0C 00
            if (cea[pos+1] == 0x03 && cea[pos+2] == 0x0C && cea[pos+3] == 0x00)
              hdmi_edid_info.hdmi_vsdb_found = true;
          }
          pos += 1 + len;
        }
      }
    }
  }

  // ── Print report ─────────────────────────────────────────────────────────
  Serial.printf("\n+-- Connected Monitor (EDID) ------------------------------------------\n");
  Serial.printf("|  Manufacturer : %-3s   Product: 0x%04X   Year: %u\n",
      hdmi_edid_info.manufacturer, hdmi_edid_info.product_code, hdmi_edid_info.year);
  if (hdmi_edid_info.monitor_name[0])
    Serial.printf("|  Name         : %s\n", hdmi_edid_info.monitor_name);
  if (hdmi_edid_info.preferred_pclk_khz)
    Serial.printf("|  Preferred    : %ux%u  (pclk=%.1f MHz)\n",
        hdmi_edid_info.preferred_hactive, hdmi_edid_info.preferred_vactive,
        hdmi_edid_info.preferred_pclk_khz / 1000.0f);
  Serial.printf("|  Signal type  : %s\n", hdmi_edid_info.hdmi_vsdb_found ? "HDMI" : "DVI");
  if (hdmi_edid_info.std_timing_count) {
    Serial.printf("|  Std timings  :");
    for (int i = 0; i < hdmi_edid_info.std_timing_count; i++)
      Serial.printf("  %ux%u", hdmi_edid_info.std_hactive[i], hdmi_edid_info.std_vactive[i]);
    Serial.println();
  }
  if (hdmi_edid_info.cea_vic_count) {
    Serial.printf("|  CEA VICs     :");
    for (int i = 0; i < hdmi_edid_info.cea_vic_count; i++)
      Serial.printf(" %u", hdmi_edid_info.cea_vic[i]);
    Serial.println();
  }
  Serial.println("|");
  Serial.println("|  WLED mode vs EDID (Y = resolution present in EDID data):");
  for (int m = 0; m < (int)HDMI_MODE_COUNT; m++) {
    const hdmi_cea861_entry_t &t = hdmi_cea861_table[m];
    bool match = (t.width == hdmi_edid_info.preferred_hactive &&
                  t.height == hdmi_edid_info.preferred_vactive);
    for (int s = 0; s < hdmi_edid_info.std_timing_count && !match; s++)
      match = (t.width == hdmi_edid_info.std_hactive[s] &&
               t.height == hdmi_edid_info.std_vactive[s]);
    for (int v = 0; v < hdmi_edid_info.cea_vic_count && !match; v++)
      if (t.vic && t.vic == hdmi_edid_info.cea_vic[v]) match = true;
    Serial.printf("|    [%c] %s\n", match ? 'Y' : ' ', hdmi_mode_names[m]);
  }
  Serial.println("+----------------------------------------------------------------------\n");

  // ── Apply HDMI/DVI mode bit to LT8912B ───────────────────────────────────
  // Reg 0xB2 bit 0: 0=DVI, 1=HDMI. The LT8912B datasheet says it can't auto-detect
  // this via DDC, so we set it manually based on the CEA-861 HDMI VSDB presence.
  if (lt8912b_io_main) {
    uint8_t b2 = lt8912b_read_reg(lt8912b_io_main, 0xB2);
    uint8_t target = hdmi_edid_info.hdmi_vsdb_found ? (b2 | 0x01) : (b2 & ~0x01);
    if (target != b2) {
      lt8912b_write_reg(lt8912b_io_main, 0xB2, target);
      USER_PRINTF("EDID: LT8912B 0xB2: 0x%02X → 0x%02X (%s mode)\n",
          b2, target, hdmi_edid_info.hdmi_vsdb_found ? "HDMI" : "DVI");
    } else {
      USER_PRINTF("EDID: LT8912B 0xB2=0x%02X (%s mode — already correct)\n",
          b2, hdmi_edid_info.hdmi_vsdb_found ? "HDMI" : "DVI");
    }
  }
}

// ============================================================
// Tear down the HDMI display stack (panel → I2C handles → DSI bus).
// NULLs both FB pointers first so the blit loop stops and the swap can't resurrect a stale pointer.
// ============================================================
static void hdmi_display_deinit() {
  // Take busMutex to wait for any in-progress blit (including draw_bitmap) to fully complete.
  // This ensures we know exactly when the last GDMA-backed vsync flip was issued.
  xSemaphoreTake(busMutex, pdMS_TO_TICKS(500));
  display_framebuffer       = NULL;
  display_front_framebuffer = NULL;
  xSemaphoreGive(busMutex);

  // Wait two frame periods (40ms @ 50Hz) for the last draw_bitmap's GDMA vsync interrupt to fire
  // and be handled before panel_del gates the GDMA clock.
  vTaskDelay(pdMS_TO_TICKS(50));

  // Delete panel first (it may use I2C handles during teardown)
  if (panel_handle)    { esp_lcd_panel_del(panel_handle);           panel_handle    = NULL; }
  vTaskDelay(pdMS_TO_TICKS(50));  // let any in-flight vsync/DMA interrupt drain before freeing the bus

  // Delete I2C handles after panel is gone
  if (lt8912b_io_avi)  { esp_lcd_panel_io_del(lt8912b_io_avi);   lt8912b_io_avi  = NULL; }
  if (lt8912b_io_cec)  { esp_lcd_panel_io_del(lt8912b_io_cec);   lt8912b_io_cec  = NULL; }
  if (lt8912b_io_main) { esp_lcd_panel_io_del(lt8912b_io_main);  lt8912b_io_main = NULL; }

  // Delete DSI bus last (panel depends on it)
  if (lt8912b_dsi_bus) { esp_lcd_del_dsi_bus(lt8912b_dsi_bus);   lt8912b_dsi_bus = NULL; }

  // Free our dedicated I2C master bus
  if (hdmi_i2c_bus) { i2c_del_master_bus(hdmi_i2c_bus); hdmi_i2c_bus = NULL; }

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
  bus_config.phy_clk_src    = MIPI_DSI_PHY_CLK_SRC_DEFAULT;

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
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(hdmi_i2c_bus, &io_cfg_main, &io_main));
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(hdmi_i2c_bus, &io_cfg_cec,  &io_cec));
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(hdmi_i2c_bus, &io_cfg_avi,  &io_avi));
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

  // Read and parse EDID from the connected monitor.
  // Must happen after HDMI link lock (DDC channel is only live once HPD is asserted).
  hdmi_edid_init();

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
// hdmi_edid_best_mode() — pick the best mode from HDMI_MODE_LIST based on EDID.
// Scoring: width*height*fps, with priority bonuses:
//   +2 000 000  preferred-timing exact match
//   +1 000 000  CEA VIC match
//   +0          standard-timing match
// Returns mode index, or -1 if EDID not present / no supported mode found.
// ============================================================
static int hdmi_edid_best_mode() {
  if (!hdmi_edid_info.present) return -1;

  int      best_mode  = -1;
  uint32_t best_score = 0;

  for (int m = 0; m < (int)HDMI_MODE_COUNT; m++) {
    const hdmi_cea861_entry_t &t = hdmi_cea861_table[m];
    uint32_t score = 0;

    // 1. Preferred timing exact match (highest priority)
    if (t.width == hdmi_edid_info.preferred_hactive &&
        t.height == hdmi_edid_info.preferred_vactive)
      score = (uint32_t)t.width * t.height * t.fps + 2000000u;

    // 2. CEA VIC match
    if (!score && t.vic) {
      for (int v = 0; v < hdmi_edid_info.cea_vic_count; v++) {
        if (t.vic == hdmi_edid_info.cea_vic[v]) {
          score = (uint32_t)t.width * t.height * t.fps + 1000000u;
          break;
        }
      }
    }

    // 3. Standard timing resolution match
    if (!score) {
      for (int s = 0; s < hdmi_edid_info.std_timing_count; s++) {
        if (t.width == hdmi_edid_info.std_hactive[s] &&
            t.height == hdmi_edid_info.std_vactive[s]) {
          score = (uint32_t)t.width * t.height * t.fps;
          break;
        }
      }
    }

    if (score > best_score) {
      best_score = score;
      best_mode  = m;
    }
  }
  return best_mode;
}

// ============================================================
// hdmi_setup() — boot-time init. Use 'm' in serial console to switch modes at runtime.
// Call once from WLED::setup().
// ============================================================
void hdmi_setup() {
  // WLEDMM: ensure our dedicated I2C master bus is up before the HDMI bridge
  // needs it.  The LT8912B communicates via I2C at 0x48/0x49/0x4A (register
  // spaces) and 0x50 (DDC EDID proxy).  We create our own port-1 master bus
  // here so we don't have to coordinate with whatever the rest of WLED does
  // (or doesn't do) to populate global_i2c_bus_handle.
  if (!hdmi_i2c_bus) {
    USER_PRINTLN("HDMI: initializing I2C bus for LT8912B bridge");
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port          = (i2c_port_t)1;     // port 1 — leaves util.cpp's transient scan and audioreactive alone
    bus_cfg.sda_io_num        = (gpio_num_t)HW_PIN_SDA;
    bus_cfg.scl_io_num        = (gpio_num_t)HW_PIN_SCL;
    bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    esp_err_t br = i2c_new_master_bus(&bus_cfg, &hdmi_i2c_bus);
    if (br != ESP_OK || hdmi_i2c_bus == NULL) {
      USER_PRINTF("HDMI: i2c_new_master_bus failed (err %d) — HDMI disabled\n", br);
      return;
    }
  }

  int selected = (int)WLEDMM_DISPLAY_MODE;
  Serial.printf("HDMI: starting mode %d: %s (type 'm' to switch)\n", selected, hdmi_mode_names[selected]);
  hdmi_display_init(selected);

  // After the first init the EDID has been read — check if a better-matching mode exists.
  int best = hdmi_edid_best_mode();
  if (best >= 0 && best != selected) {
    Serial.printf("HDMI: EDID recommends mode %d: %s — switching\n", best, hdmi_mode_names[best]);
    hdmi_switch_mode(best);
  } else if (best == selected) {
    Serial.printf("HDMI: EDID confirms default mode %d: %s\n", selected, hdmi_mode_names[selected]);
  }
  // best == -1: no EDID / no matching mode — stay on default
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
  // Capture globals into locals immediately — deinit may NULL the globals at any time.
  uint8_t* back_fb = display_framebuffer;
  esp_lcd_panel_handle_t ph = panel_handle;
  if (!back_fb || !ph) return;

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
          srm_cfg.out.buffer         = back_fb;
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
            // Present the back buffer at next vsync, then swap so next blit writes to the old front.
            esp_lcd_panel_draw_bitmap(ph, 0, 0, WLEDMM_DISPLAY_W, WLEDMM_DISPLAY_H, back_fb);
            uint8_t* tmp          = display_front_framebuffer;
            display_front_framebuffer = back_fb;
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

/*
 * c5_panel.cpp - see the header.
 *
 * NOT a standalone translation unit: included at the end of c5_panel.h.
 */

#include "../../wled00/src/font/console_font_6x8.h"
#include "../../wled00/src/dependencies/esp_lcd_ili9488/esp_lcd_ili9488.h"

// ---------------------------------------------------------------------------
// Bringing the panel up
// ---------------------------------------------------------------------------

bool C5Panel::begin(C5IoExpander &ioexp) {
  // The frame buffer is in PSRAM: 200 KB is a lot to ask of the chip's
  // internal RAM, and the picture is not looked at often enough for the
  // slower memory to matter.
  _fb = (uint16_t *)heap_caps_malloc((size_t)C5_PANEL_W * C5_PANEL_H * sizeof(uint16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (_fb == nullptr) {
    // No PSRAM: the picture still fits, but it is tight.
    _fb = (uint16_t *)heap_caps_malloc((size_t)C5_PANEL_W * C5_PANEL_H * sizeof(uint16_t),
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (_fb == nullptr) {
    USER_PRINTLN("C5: no memory for the panel's picture");
    return false;
  }
  memset(_fb, 0, (size_t)C5_PANEL_W * C5_PANEL_H * sizeof(uint16_t));

  // The transfer buffer has to be in internal RAM: the SPI's DMA reads it.
  // One row is enough - the panel is sent a row at a time.
  _dma = (uint16_t *)heap_caps_malloc((size_t)C5_PANEL_W * sizeof(uint16_t),
                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (_dma == nullptr) {
    USER_PRINTLN("C5: no memory for the panel's transfer buffer");
    return false;
  }

  // The SPI bus. The TF card shares it, so the card's chip select is put
  // high and left there.
  pinMode(C5_SD_CS, OUTPUT);
  digitalWrite(C5_SD_CS, HIGH);

  spi_bus_config_t buscfg = {};
  buscfg.sclk_io_num = C5_LCD_SCLK;
  buscfg.mosi_io_num = C5_LCD_MOSI;
  buscfg.miso_io_num = C5_LCD_MISO;
  buscfg.quadwp_io_num = GPIO_NUM_NC;
  buscfg.quadhd_io_num = GPIO_NUM_NC;
  buscfg.max_transfer_sz = C5_PANEL_W * 64 * 2;
  esp_err_t ret = spi_bus_initialize(C5_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    USER_PRINTF("C5: the panel's SPI bus did not come up (0x%x)\n", (int)ret);
    return false;
  }

  esp_lcd_panel_io_spi_config_t io_config = {};
  io_config.dc_gpio_num = C5_LCD_DC;
  io_config.cs_gpio_num = C5_LCD_CS;
  io_config.pclk_hz = 60 * 1000 * 1000;
  io_config.lcd_cmd_bits = 8;
  io_config.lcd_param_bits = 8;
  io_config.spi_mode = 0;
  io_config.trans_queue_depth = 10;
  ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)C5_SPI_HOST, &io_config, &_io);
  if (ret != ESP_OK) {
    USER_PRINTF("C5: the panel's IO did not come up (0x%x)\n", (int)ret);
    return false;
  }

  // 16 bits per pixel: the picture IS RGB565, so the data goes out as it
  // is, with no conversion in between.
  esp_lcd_panel_dev_config_t panel_config = {};
  panel_config.reset_gpio_num = GPIO_NUM_NC;   // the reset is on the IO expander
  panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
  panel_config.bits_per_pixel = 16;
  panel_config.data_endian = LCD_RGB_DATA_ENDIAN_BIG;

  ret = esp_lcd_new_panel_ili9488(_io, &panel_config, 0, &_panel);
  if (ret != ESP_OK) {
    USER_PRINTF("C5: the panel did not come up (0x%x)\n", (int)ret);
    return false;
  }

  // THE RESET, through the IO expander: the panel's reset line is IO0
  // there, not a GPIO of this chip.
  ioexp.setPin(C5_IOEXP_LCD_RST, false);
  delay(20);
  ioexp.setPin(C5_IOEXP_LCD_RST, true);
  delay(150);

  esp_lcd_panel_init(_panel);
  esp_lcd_panel_invert_color(_panel, true);
  esp_lcd_panel_swap_xy(_panel, false);
  esp_lcd_panel_mirror(_panel, false, false);
  esp_lcd_panel_disp_on_off(_panel, true);

  // The backlight. The vendor caps the duty at 97 %.
  ioexp.setPwm(97);

  markAllDirty();
  USER_PRINTLN("C5: the panel is up (320x320, ILI9488 over SPI)");
  return true;
}

void C5Panel::end() {
  if (_panel) {
    esp_lcd_panel_del(_panel);
    _panel = nullptr;
  }
  if (_fb) {
    heap_caps_free(_fb);
    _fb = nullptr;
  }
  if (_dma) {
    heap_caps_free(_dma);
    _dma = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

inline void C5Panel::plot(int x, int y, uint16_t color) {
  if (x < 0 || y < 0 || x >= C5_PANEL_W || y >= C5_PANEL_H) return;
  _fb[(size_t)y * C5_PANEL_W + x] = color;
}

void C5Panel::clear(uint16_t color) {
  if (_fb == nullptr) return;
  for (size_t i = 0; i < (size_t)C5_PANEL_W * C5_PANEL_H; i++) _fb[i] = color;
  markAllDirty();
}

void C5Panel::fillRect(int x, int y, int w, int h, uint16_t color) {
  if (_fb == nullptr || w <= 0 || h <= 0) return;
  int x0 = max(0, x), y0 = max(0, y);
  int x1 = min(C5_PANEL_W, x + w), y1 = min(C5_PANEL_H, y + h);
  for (int yy = y0; yy < y1; yy++) {
    uint16_t *row = &_fb[(size_t)yy * C5_PANEL_W];
    for (int xx = x0; xx < x1; xx++) row[xx] = color;
  }
  markDirty(x, y, w, h);
}

void C5Panel::drawRect(int x, int y, int w, int h, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  fillRect(x, y, w, 1, color);
  fillRect(x, y + h - 1, w, 1, color);
  fillRect(x, y, 1, h, color);
  fillRect(x + w - 1, y, 1, h, color);
}

void C5Panel::drawHLine(int x, int y, int w, uint16_t color) { fillRect(x, y, w, 1, color); }
void C5Panel::drawVLine(int x, int y, int h, uint16_t color) { fillRect(x, y, 1, h, color); }

int C5Panel::textWidth(const char *text) {
  if (text == nullptr) return 0;
  int n = 0;
  while (text[n]) n++;
  return n > 0 ? n * C5_FONT_ADVANCE - 1 : 0;
}

void C5Panel::drawText(int x, int y, const char *text, uint16_t fg, uint16_t bg) {
  if (_fb == nullptr || text == nullptr) return;
  int gx = x;
  for (const char *p = text; *p; p++, gx += C5_FONT_ADVANCE) {
    unsigned char ch = (unsigned char)*p;
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t *glyph = &console_font_6x8[(ch - 32) * C5_FONT_H];
    for (int row = 0; row < C5_FONT_H; row++) {
      uint8_t bits = pgm_read_byte(&glyph[row]);
      for (int col = 0; col < C5_FONT_W; col++) {
        // The leftmost pixel of the glyph is the top bit of the row.
        plot(gx + col, y + row, ((bits >> (7 - col)) & 1) ? fg : bg);
      }
    }
  }
  markDirty(x, y, textWidth(text), C5_FONT_H);
}

void C5Panel::drawTextRight(int xRight, int y, const char *text, uint16_t fg, uint16_t bg) {
  drawText(xRight - textWidth(text), y, text, fg, bg);
}

void C5Panel::drawTextCentre(int xCentre, int y, const char *text, uint16_t fg, uint16_t bg) {
  drawText(xCentre - textWidth(text) / 2, y, text, fg, bg);
}

// ---------------------------------------------------------------------------
// Getting it onto the panel
// ---------------------------------------------------------------------------

void C5Panel::markDirty(int x, int y, int w, int h) {
  if (_fb == nullptr) return;
  int x0 = max(0, x), y0 = max(0, y);
  int x1 = min(C5_PANEL_W - 1, x + w - 1), y1 = min(C5_PANEL_H - 1, y + h - 1);
  if (x1 < x0 || y1 < y0) return;
  if (!_dirty) {
    _dx0 = x0; _dy0 = y0; _dx1 = x1; _dy1 = y1;
    _dirty = true;
    _nextRow = _dy0;
  } else {
    if (x0 < _dx0) _dx0 = x0;
    if (y0 < _dy0) _dy0 = y0;
    if (x1 > _dx1) _dx1 = x1;
    if (y1 > _dy1) _dy1 = y1;
  }
}

void C5Panel::markAllDirty() {
  if (_fb == nullptr) return;
  _dx0 = 0; _dy0 = 0; _dx1 = C5_PANEL_W - 1; _dy1 = C5_PANEL_H - 1;
  _dirty = true;
  _nextRow = 0;
}

bool C5Panel::sendRows(int x, int y0, int y1, int x1) {
  // One row at a time. The panel's address counter does wrap from the end
  // of a row to the start of the next one, but a row at a time does not
  // need that to be true, and a row is small enough that the command
  // overhead does not matter.
  const int w = x1 - x + 1;
  for (int row = y0; row <= y1; row++) {
    memcpy(_dma, &_fb[(size_t)row * C5_PANEL_W + x], (size_t)w * sizeof(uint16_t));
    esp_err_t ret = esp_lcd_panel_draw_bitmap(_panel, x, row, x1 + 1, row + 1, _dma);
    if (ret != ESP_OK) return false;
  }
  return true;
}

void C5Panel::flush(unsigned budgetMs) {
  if (_panel == nullptr || !_dirty) return;

  const int64_t start = esp_timer_get_time();
  // The rows go in order, and the flush picks up where it left off when the
  // budget ran out - so a full screen goes out over a few loops instead of
  // stalling one of them.
  while (_nextRow <= _dy1) {
    if (!sendRows(_dx0, _nextRow, _nextRow, _dx1)) {
      _dirty = false;
      return;
    }
    _nextRow++;
    if ((esp_timer_get_time() - start) / 1000 >= (int64_t)budgetMs) break;
  }

  if (_nextRow > _dy1) {
    _dirty = false;
    _nextRow = 0;
  }
}

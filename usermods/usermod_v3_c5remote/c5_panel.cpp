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
  const size_t pixels = (size_t)C5_PANEL_W * C5_PANEL_H * sizeof(uint16_t);
  _fb = (uint16_t *)heap_caps_malloc(pixels, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (_fb == nullptr) {
    // No PSRAM: the picture still fits, but it is tight.
    _fb = (uint16_t *)heap_caps_malloc(pixels, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  // The second picture: the panel is sent one while the next is drawn.
  _fbShow = (uint16_t *)heap_caps_malloc(pixels, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (_fbShow == nullptr) {
    _fbShow = (uint16_t *)heap_caps_malloc(pixels, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (_fb == nullptr || _fbShow == nullptr) {
    USER_PRINTLN("C5: no memory for the panel's picture");
    return false;
  }
  memset(_fb, 0, pixels);
  memset(_fbShow, 0, pixels);

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

  _frameReady = true;
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
  _frameReady = true;
}

void C5Panel::fillRect(int x, int y, int w, int h, uint16_t color) {
  if (_fb == nullptr || w <= 0 || h <= 0) return;
  int x0 = max(0, x), y0 = max(0, y);
  int x1 = min(C5_PANEL_W, x + w), y1 = min(C5_PANEL_H, y + h);
  for (int yy = y0; yy < y1; yy++) {
    uint16_t *row = &_fb[(size_t)yy * C5_PANEL_W];
    for (int xx = x0; xx < x1; xx++) row[xx] = color;
  }
  _frameReady = true;
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
  _frameReady = true;
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

bool C5Panel::sendRows(int x, int y0, int y1, int x1, const uint16_t *fb) {
  // One row at a time. The panel's address counter does wrap from the end
  // of a row to the start of the next one, but a row at a time does not
  // need that to be true, and a row is small enough that the command
  // overhead does not matter.
  const int w = x1 - x + 1;
  for (int row = y0; row <= y1; row++) {
    memcpy(_dma, &fb[(size_t)row * C5_PANEL_W + x], (size_t)w * sizeof(uint16_t));
    // The bytes of each pixel go out the other way round.
    //
    // The picture is held as the chip holds it - little-endian, so the low
    // byte (the blue) is first - and the panel takes the high byte first
    // (the red).  In 18 bit mode the driver does the conversion and gets
    // this right; in 16 bit mode it hands the picture over as it is, so
    // the swap is ours to do.
    {
      uint8_t *d = (uint8_t *)_dma;
      for (int i = 0; i < w; i++) {
        const uint8_t t = d[0]; d[0] = d[1]; d[1] = t;
        d += 2;
      }
    }
    esp_err_t ret = esp_lcd_panel_draw_bitmap(_panel, x, row, x1 + 1, row + 1, _dma);
    if (ret != ESP_OK) {
      if (_errors < 8) {
        USER_PRINTF("C5: the panel refused a row (%d)\n", (int)ret);
        _errors++;
      }
      return false;
    }
  }
  return true;
}

// Sending a frame.
//
// The picture that is sent is the one that was finished, and the drawing
// goes into the other buffer, so the panel never sees half of one picture
// and half of the next.
//
// The rows go in order, and the send picks up where it left off when the
// budget ran out - so a full screen goes out over a few loops instead of
// stalling one of them.
void C5Panel::flush(unsigned budgetMs) {
  if (_panel == nullptr) return;

  // A frame that is on its way keeps going.
  if (!_sending) {
    if (!_frameReady) return;
    // The picture that was drawn is the one to send; the other buffer is
    // where the next one goes.
    uint16_t *swap = _fbShow;
    _fbShow = _fb;
    _fb = swap;
    _frameReady = false;
    _sending = true;
    _sendRow = 0;
    _sendPtr = _fbShow;
  }

  const int64_t start = esp_timer_get_time();
  while (_sendRow < C5_PANEL_H) {
    if (!sendRows(0, _sendRow, _sendRow, C5_PANEL_W - 1, _sendPtr)) {
      _sending = false;
      return;
    }
    _sendRow++;
    if ((esp_timer_get_time() - start) / 1000 >= (int64_t)budgetMs) return;
  }
  _sending = false;
}

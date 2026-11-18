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
  // The third: what the panel has.  It is what the new picture is
  // compared against, so that the bands which have not changed are not
  // sent again.
  _fbPanel = (uint16_t *)heap_caps_malloc(pixels, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (_fbPanel == nullptr) {
    _fbPanel = (uint16_t *)heap_caps_malloc(pixels, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (_fb == nullptr || _fbShow == nullptr || _fbPanel == nullptr) {
    USER_PRINTLN("C5: no memory for the panel's picture");
    return false;
  }
  memset(_fb, 0, pixels);
  memset(_fbShow, 0, pixels);
  // The panel starts with nothing on it, so the first frame sends
  // everything: the picture it has is not the picture it is showing.
  memset(_fbPanel, 0, pixels);

  // Where the pictures ended up.  Two hundred KB each is a lot to ask of
  // the internal heap, and if PSRAM is not there to take it, the web
  // server's own memory is gone.
  USER_PRINTF("C5: the picture: %u KB, %u KB free of PSRAM\n",
              (unsigned)(pixels / 1024), (unsigned)ESP.getFreePsram() / 1024);

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
  // The biggest thing that goes over the bus is a band of rows, in the
  // panel's own 18 bit format: three bytes a pixel.
  buscfg.max_transfer_sz = C5_PANEL_W * C5_PANEL_BAND_ROWS * 3;
  esp_err_t ret = spi_bus_initialize(C5_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    USER_PRINTF("C5: the panel's SPI bus did not come up (0x%x)\n", (int)ret);
    return false;
  }
  USER_PRINTF("C5: the panel's SPI bus: 0x%x\n", (int)ret);

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

  // 18 bits per pixel: this is what the vendor's own demo for this board
  // uses, and it is known to work on it.  The driver converts the picture
  // from RGB565 to RGB666 on the way out, so the picture is still RGB565
  // in the frame buffer and the conversion is the driver's business.
  //
  // 16 bit was tried first and gave a grey screen: the panel was told
  // COLMOD=0x55 and then fed RGB565 bytes, and this panel does not show
  // that.  The vendor's way is the way that works.
  esp_lcd_panel_dev_config_t panel_config = {};
  panel_config.reset_gpio_num = GPIO_NUM_NC;   // the reset is on the IO expander
  panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
  panel_config.bits_per_pixel = 18;
  panel_config.data_endian = LCD_RGB_DATA_ENDIAN_BIG;

  // The conversion buffer is a band's worth of pixels.  The driver
  // converts a whole rectangle at a time and will not take one larger
  // than this, so the picture goes out a band at a time.
  ret = esp_lcd_new_panel_ili9488(_io, &panel_config, C5_PANEL_W * C5_PANEL_BAND_ROWS, &_panel);
  USER_PRINTF("C5: the panel handle: 0x%x\n", (int)ret);
  if (ret != ESP_OK) {
    USER_PRINTF("C5: the panel did not come up (0x%x)\n", (int)ret);
    return false;
  }

  // THE RESET, through the IO expander: the panel's reset line is IO0
  // there, not a GPIO of this chip.  It is active low: low for 20 ms,
  // then released, and the panel is given 150 ms to wake up.
  ioexp.setPin(C5_IOEXP_LCD_RST, false);
  delay(20);
  ioexp.setPin(C5_IOEXP_LCD_RST, true);
  delay(150);

  // Every one of these is checked and said out loud.  A panel that was
  // never initialised shows as a flat grey with the backlight on, and
  // that is exactly what it looks like, so the console has to say
  // whether each step worked.
  ret = esp_lcd_panel_reset(_panel);
  USER_PRINTF("C5: the panel's reset: 0x%x\n", (int)ret);
  ret = esp_lcd_panel_init(_panel);
  USER_PRINTF("C5: the panel's init sequence: 0x%x\n", (int)ret);
  ret = esp_lcd_panel_invert_color(_panel, true);
  USER_PRINTF("C5: the panel's invert: 0x%x\n", (int)ret);
  ret = esp_lcd_panel_swap_xy(_panel, false);
  USER_PRINTF("C5: the panel's swap: 0x%x\n", (int)ret);
  ret = esp_lcd_panel_mirror(_panel, false, false);
  USER_PRINTF("C5: the panel's mirror: 0x%x\n", (int)ret);
  ret = esp_lcd_panel_disp_on_off(_panel, true);
  USER_PRINTF("C5: the panel's display on: 0x%x\n", (int)ret);

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
  if (_fbShow) {
    heap_caps_free(_fbShow);
    _fbShow = nullptr;
  }
  if (_fbPanel) {
    heap_caps_free(_fbPanel);
    _fbPanel = nullptr;
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
  // The picture is 320 x 320, and it lives in PSRAM.  Clearing it a pixel
  // at a time is a hundred thousand writes across the memory bus, and it
  // is slow enough to be seen: the screen takes longer to draw than to
  // send.  Two pixels at a time is half the writes, and the compiler
  // turns the loop into a wide store.
  uint32_t *p = (uint32_t *)_fb;
  const uint32_t pattern = (uint32_t)color | ((uint32_t)color << 16);
  const size_t n = (size_t)C5_PANEL_W * C5_PANEL_H / 2;
  for (size_t i = 0; i < n; i++) p[i] = pattern;
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
  // A band of rows at a time.
  //
  // The panel's address counter wraps from the end of a row to the start
  // of the next one, so a rectangle of several rows can be sent as one
  // block: the panel walks the window on its own.  The driver converts
  // the picture to what the panel takes, so the picture that goes in is
  // the frame buffer itself, in RGB565, and no copy is needed.
  //
  // The window is set for EVERY band, and that is not an accident: the
  // panel starts each block of pixels at the start of the window, so a
  // window set once for the whole screen has every band land on the same
  // rows.  The screen came out grey with a picture that never went past
  // the top.  Setting it per band is what the panel wants.
  //
  // A row at a time was tried first and was far too slow: the window has
  // to be set before every block of pixels, and each of those is a round
  // trip that waits for the one before it.  Six hundred and forty of them
  // for one screen, and the screen took a second.  A band at a time is
  // forty.
  esp_err_t ret = esp_lcd_panel_draw_bitmap(_panel, x, y0, x1 + 1, y1 + 1,
                                            &fb[(size_t)y0 * C5_PANEL_W + x]);
  if (ret != ESP_OK) {
    if (_errors < 8) {
      USER_PRINTF("C5: the panel refused a band (%d)\n", (int)ret);
      _errors++;
    }
    return false;
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
//
// Only the bands that CHANGED are sent.  The panel keeps what it was last
// given, so a band that is the same as it was needs no wire time at all -
// and most of a page is the same from one look to the next: only the
// numbers and the meter move.  A full screen is 300 KB on the wire and a
// band is 7.5 KB, so sending three bands instead of forty is the
// difference between a screen that keeps up and one that does not.
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
    _frameStart = esp_timer_get_time();
    // Which bands are different from the ones the panel already has.
    //
    // The panel's own copy is the picture it was last sent, and that is
    // what the difference is against.  The first frame has no such
    // picture - the panel is blank - so everything is sent.
    if (!_sentAnything) {
      _dirty = 0xFFFFFFFF;
    } else {
      _dirty = 0;
      for (uint8_t b = 0; b < C5_PANEL_BAND_COUNT; b++) {
        const size_t rows = min(C5_PANEL_BAND_ROWS, C5_PANEL_H - (int)b * C5_PANEL_BAND_ROWS);
        const size_t bytes = rows * C5_PANEL_W * sizeof(uint16_t);
        if (memcmp(&_fbShow[(size_t)b * C5_PANEL_BAND_ROWS * C5_PANEL_W],
                   &_fbPanel[(size_t)b * C5_PANEL_BAND_ROWS * C5_PANEL_W], bytes) != 0) {
          _dirty |= (uint32_t)1 << b;
        }
      }
    }
  }

  const int64_t start = esp_timer_get_time();
  while (_sendRow < C5_PANEL_H) {
    const int band = _sendRow / C5_PANEL_BAND_ROWS;
    const int last = min(_sendRow + C5_PANEL_BAND_ROWS - 1, C5_PANEL_H - 1);

    // A band the panel already has is not sent again.
    if (_dirty & ((uint32_t)1 << band)) {
      const int64_t bandStart = esp_timer_get_time();
      if (!sendRows(0, _sendRow, last, C5_PANEL_W - 1, _sendPtr)) {
        _sending = false;
        return;
      }
      // The panel has this band now, so the next frame compares against it.
      const size_t rows = (size_t)(last - _sendRow + 1);
      memcpy(&_fbPanel[(size_t)_sendRow * C5_PANEL_W],
             &_fbShow[(size_t)_sendRow * C5_PANEL_W],
             rows * C5_PANEL_W * sizeof(uint16_t));
      _bandMs += (uint32_t)((esp_timer_get_time() - bandStart) / 1000);
      _bandCount++;
    }
    _sendRow = last + 1;
    if ((esp_timer_get_time() - start) / 1000 >= (int64_t)budgetMs) return;
  }
  _sending = false;
  _sentAnything = true;
  _frameMs = (uint32_t)((esp_timer_get_time() - _frameStart) / 1000);
  _frameCount++;
}

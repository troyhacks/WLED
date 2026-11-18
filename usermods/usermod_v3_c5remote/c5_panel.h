/*
 * c5_panel.h - the 2.73" 320x320 ILI9488 panel on the ESP32-C5 remote.
 *
 * This is NOT WLEDMM's display layer. That one is the P4's: a DSI/PPA
 * pipeline that scales the LED bus into a monitor's framebuffer, with no
 * drawing primitives in it at all. This board has an SPI panel on a
 * different chip, so it gets its own.
 *
 * HOW IT IS PUT TOGETHER
 * ----------------------
 * The picture lives in a frame buffer in PSRAM (320 x 320 x 2 bytes, RGB565),
 * the drawing happens there, and only the part that changed is sent to the
 * panel. The transfer goes through a small buffer in internal RAM, because
 * the SPI's DMA cannot read the frame buffer where it is.
 *
 * The panel itself is driven by ESP-IDF's esp_lcd, with the ILI9488's panel
 * driver in wled00/src/dependencies/esp_lcd_ili9488.
 *
 * The panel's RESET line is on the IO expander, not on a GPIO of the chip,
 * so the expander is what resets it.
 *
 * The text is WLED's own console font (wled00/src/font/console_font_6x8.h) -
 * 6 pixels wide, 8 tall, one byte per row, the leftmost pixel in the top
 * bit. The same font the scrolling text effect uses.
 */

#pragma once

#include "wled.h"

#if defined(WLEDMM_C5_DISPLAY)

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

#include "c5_board.h"

// The panel's size
#define C5_PANEL_W 320
#define C5_PANEL_H 320

// How many rows go out at a time.
//
// The panel's window is set ONCE for the whole frame and the picture is
// then streamed into it.  The panel's address counter walks the window on
// its own, so the rows do not each need a window of their own - which is
// what made the screen crawl: six hundred and forty round trips for one
// screen, each one waiting for the last.
//
// The picture goes out in bands because a band is what fits in the
// transfer buffer, and there are two of them so that the conversion of
// one band happens while the other is on its way out.
#define C5_PANEL_BAND_ROWS 8
#define C5_PANEL_BANDS 2


// The screen comes over HTTP in tiles of this many rows.
#define C5_TILE_ROWS 40

// The console font: 6 wide, 8 tall, and one pixel of space between the
// characters.
#define C5_FONT_W      6
#define C5_FONT_H      8
#define C5_FONT_ADVANCE (C5_FONT_W + 1)

// A few colours, in RGB565.
#define C5_COL_BLACK   0x0000
#define C5_COL_WHITE   0xFFFF
#define C5_COL_RED     0xF800
#define C5_COL_GREEN   0x07E0
#define C5_COL_BLUE    0x001F
#define C5_COL_CYAN    0x07FF
#define C5_COL_YELLOW  0xFFE0
#define C5_COL_MAGENTA 0xF81F
#define C5_COL_GREY    0x8410
#define C5_COL_DARKGREY 0x4208
#define C5_COL_DARKBLUE 0x0010
#define C5_COL_ORANGE  0xFD20

// A colour from its parts, so a bar can fade without a table of colours.
static inline uint16_t c5_rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

class C5Panel {
  public:
    // Brings the panel up. The IO expander does the reset - the panel's
    // reset line is on the expander, not on a GPIO.
    bool begin(C5IoExpander &ioexp);
    void end();
    bool ready() const { return _panel != nullptr && _fb != nullptr; }

    // The picture that is on the panel, for the screen snapshot over HTTP.
    //
    // It is the FINISHED picture, not the one being drawn into: the
    // snapshot is taken a row at a time over eight requests, and a picture
    // that is being drawn into changes under it.
    const uint16_t *framebuffer() const { return _fbShow ? _fbShow : _fb; }

    // ---- drawing (into the frame buffer) ----
    void clear(uint16_t color);
    void fillRect(int x, int y, int w, int h, uint16_t color);
    void drawRect(int x, int y, int w, int h, uint16_t color);
    void drawHLine(int x, int y, int w, uint16_t color);
    void drawVLine(int x, int y, int h, uint16_t color);
    void drawText(int x, int y, const char *text, uint16_t fg, uint16_t bg);
    void drawTextRight(int xRight, int y, const char *text, uint16_t fg, uint16_t bg);
    void drawTextCentre(int xCentre, int y, const char *text, uint16_t fg, uint16_t bg);
    static int textWidth(const char *text);
    static int textHeight() { return C5_FONT_H; }

    // ---- and getting it onto the panel ----
    // Sends what changed. The budget is there so a full screen does not
    // stall WLED's main loop for a whole frame.
    void flush(unsigned budgetMs = 12);

    // How long the last frame took to go out, in milliseconds.  The
    // picture is drawn separately, so this is the transfer on its own.
    uint32_t lastFrameMs() const { return _frameMs; }

    // How many frames have gone out since the board came up.
    uint32_t frameCount() const { return _frameCount; }

    // How long the last frame's BANDS took, in milliseconds, and how many
    // there were.  The frame takes longer than this if the main loop was
    // busy in between, and that difference is worth knowing about.
    uint32_t lastBandMs() const { return _bandMs; }
    uint32_t lastBandCount() const { return _bandCount; }

  private:
    void plot(int x, int y, uint16_t color);
    bool sendRows(int x, int y0, int y1, int x1, const uint16_t *fb);

    esp_lcd_panel_handle_t _panel = nullptr;
    esp_lcd_panel_io_handle_t _io = nullptr;
    // TWO picture buffers.
    //
    // The drawing goes into one and the panel is sent the other, and they
    // are swapped when a frame is finished.  With one buffer the panel can
    // be sent half of one picture and half of the next - the screen tears,
    // and a page that changes (the page does) shows as a mixture of both.
    uint16_t *_fb = nullptr;      // what is drawn into, in PSRAM
    uint16_t *_fbShow = nullptr;  // what is sent, in PSRAM
    bool _frameReady = false;    // there is a picture waiting to be sent
    bool _sending = false;       // and one is on its way
    int  _sendRow = 0;           // how far it has got
    const uint16_t *_sendPtr = nullptr;  // the picture being sent
    uint32_t _errors = 0;      // how many times the panel refused a row
    uint32_t _frameMs = 0;      // how long the last frame took to go out
    int64_t  _frameStart = 0;   // when the frame on its way started
    uint32_t _frameCount = 0;   // how many frames have gone out
    uint32_t _bandMs = 0;       // how long the last frame's bands took
    uint32_t _bandCount = 0;    // and how many there were
};

#include "c5_panel.cpp"

#endif  // WLEDMM_C5_DISPLAY

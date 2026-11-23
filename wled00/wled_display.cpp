// The display subsystem.
//
// The P4 can drive a MIPI-DSI panel on its connector, or an LT8912B bridge to
// a monitor.  Which one is there is not known at build time, so nothing here
// is: the output is picked at run time, from the settings page or from what
// the I2C bus answers.
//
// The picture is scaled into whatever frame the output has, in ONE PPA pass,
// and centred.  A matrix that is far smaller than the panel is blown up as
// far as one pass can and then centred - it is letterboxed, not refused.  A
// matrix that is far larger is cropped in the middle, in the same pass.
//
// The on-screen interface (a status bar and a grid of preset buttons) is
// drawn by the CPU into the framebuffer, with the console font the rest of
// WLED already uses.

#include "wled.h"
#include "wled_display.h"
#include "wled_display_panels.h"
#include "wled_hdmi.h"

#if defined(CONFIG_IDF_TARGET_ESP32P4)

#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_ili9881c.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_st7703.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "src/dependencies/esp_lcd_jd9365/esp_lcd_jd9365.h"
#include "esp_cache.h"
#include "esp_ldo_regulator.h"
#include "src/font/console_font_6x8.h"

// ============================================================
// The state
// ============================================================

// What is attached, and what it is called.
static display_kind_t  dispKind      = DISPLAY_NONE;
static const dsi_panel_t* dispPanel  = nullptr;   // the DSI panel, if there is one
static const char*     dispName      = "";
static uint16_t        dispWidth     = 0;
static uint16_t        dispHeight    = 0;
static bool            dispPresent   = false;     // detected, and working
static uint8_t         dispView      = DISPLAY_VIEW_ONLY;
static uint8_t         dispRotation  = 0;         // 0, 90, 180, 270
static uint8_t         dispButtons   = 32;
static uint8_t         dispButtonCols= 8;
static uint8_t         dispEleBgr    = 0;         // the panel is told BGR

// The DSI bus and the panel on it (the bridge has its own, in wled_hdmi.cpp).
static esp_lcd_dsi_bus_handle_t dispDsiBus  = NULL;
static esp_lcd_panel_io_handle_t dispDbiIo  = NULL;

// The touch controller.
static esp_lcd_touch_handle_t    dispTouch   = NULL;
static esp_lcd_panel_io_handle_t dispTouchIo = NULL;

// A change that was asked for and has not been carried out yet.
static volatile bool dispPendingChange = false;

// The interface's layout.  It is worked out from the panel's size, so a
// 1024x600 panel and a 720x720 one both get a sensible layout.
struct disp_layout_t {
  int barX, barY, barW, barH;
  int gridX, gridY, gridW, gridH;
  int prevX, prevY, prevW, prevH;
  int btnW, btnH, btnPad;
  uint8_t rows, cols;
};
static disp_layout_t dispLayout = {};

// The pointer (a mouse, over the HID usermod).
static int  dispPtrX = -1, dispPtrY = -1;
static bool dispPtrDown = false;
static bool dispPtrSeen = false;

// ============================================================
// The layout
// ============================================================

static void dispLayoutCalc(void) {
  disp_layout_t& L = dispLayout;
  L = {};
  if (!dispWidth || !dispHeight) return;

  const int W = dispWidth, H = dispHeight;

  // The bar is a strip along the top.  Its height follows the width, but it
  // never gets so tall that it eats the picture.
  L.barX = 0; L.barY = 0; L.barW = W;
  L.barH = W / 14;
  if (L.barH < 24) L.barH = 24;
  if (L.barH > 64) L.barH = 64;
  if (L.barH > H / 4) L.barH = H / 4;

  // The grid is along the bottom.  The buttons are as square as they can
  // be, but the grid never takes more than a third of the screen.
  const uint8_t cols = dispButtonCols ? dispButtonCols : 8;
  const uint8_t buttons = dispButtons ? dispButtons : 32;
  uint8_t rows = (uint8_t)((buttons + cols - 1) / cols);
  if (!rows) rows = 1;
  L.cols = cols;
  L.rows = rows;

  L.btnPad = 5;
  L.btnW = (W - (cols + 1) * L.btnPad) / cols;
  if (L.btnW < 8) L.btnW = 8;
  const int gridMaxH = H / 3;
  L.btnH = (gridMaxH - (rows + 1) * L.btnPad) / rows;
  if (L.btnH > L.btnW) L.btnH = L.btnW;      // square, if there is room
  if (L.btnH < 8) L.btnH = 8;

  L.gridW = cols * L.btnW + (cols + 1) * L.btnPad;
  L.gridH = rows * L.btnH + (rows + 1) * L.btnPad;
  L.gridX = 0;
  L.gridY = H - L.gridH;

  // The picture is what is left in the middle.
  L.prevX = 0;
  L.prevY = L.barH;
  L.prevW = W;
  L.prevH = L.gridY - L.barH;
  if (L.prevH < 1) { L.prevH = 1; L.prevY = 0; }
}

// ============================================================
// Drawing into the framebuffer
//
// The framebuffer is RGB888: the first byte of a pixel is the red one.  The
// PPA writes the same way (its fill and its scale both take RGB in and put
// RGB out), so the CPU and the PPA agree and the two can share the frame.
// ============================================================

static inline void dispPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  uint8_t* fb = display_framebuffer;
  if (!fb || x < 0 || y < 0 || x >= (int)dispWidth || y >= (int)dispHeight) return;
  uint8_t* p = fb + ((size_t)y * dispWidth + x) * 3;
  p[0] = r; p[1] = g; p[2] = b;
}

static void dispFillRect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
  if (w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > (int)dispWidth)  w = (int)dispWidth - x;
  if (y + h > (int)dispHeight) h = (int)dispHeight - y;
  if (w <= 0 || h <= 0) return;
  uint8_t* fb = display_framebuffer;
  if (!fb) return;
  uint8_t* row = fb + ((size_t)y * dispWidth + x) * 3;
  for (int yy = 0; yy < h; yy++) {
    for (int xx = 0; xx < w; xx++) {
      *row++ = r; *row++ = g; *row++ = b;
    }
    row += ((size_t)dispWidth - w) * 3;
  }
}

// The console font: six pixels wide, eight tall, one byte per row, the
// leftmost pixel in the top bit.
#define DISP_FONT_W 6
#define DISP_FONT_H 8

static int dispTextWidth(const char* s) {
  int n = 0;
  for (const char* p = s; *p; p++) n++;
  return n * DISP_FONT_W;
}

static void dispText(int x, int y, const char* s, uint8_t r, uint8_t g, uint8_t b) {
  if (!s) return;
  for (const char* p = s; *p; p++) {
    uint8_t ch = (uint8_t)*p;
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t* glyph = &console_font_6x8[(ch - 32) * DISP_FONT_H];
    for (uint8_t row = 0; row < DISP_FONT_H; row++) {
      const uint8_t bits = pgm_read_byte(&glyph[row]);
      if (!bits) continue;
      for (uint8_t col = 0; col < DISP_FONT_W; col++) {
        if (bits & (0x80 >> col)) dispPixel(x + col, y + row, r, g, b);
      }
    }
    x += DISP_FONT_W;
  }
}

// The cache and the framebuffer.
//
// The picture is written by the PPA and the interface by the CPU, into the
// SAME buffer - and the two do not share a cache line cleanly, because a
// line is 128 bytes and a pixel is three.
//
// So before the CPU writes a region the PPA has written, the CPU's copy of
// that region is dropped: otherwise the flush at the end of the frame
// would put the OLD picture back over the new one.  And after the CPU has
// written, its copy is pushed out, because the DMA that presents the frame
// reads the memory and not the cache.
//
// The cache work wants whole lines: the address and the size have to be
// multiples of the line, or it refuses and says so.
#define DISP_CACHE_LINE 128

static void dispCacheRegion(int x, int y, int w, int h, bool invalidate) {
  if (!display_framebuffer || !dispWidth || !dispHeight || w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > (int)dispWidth)  w = (int)dispWidth - x;
  if (y + h > (int)dispHeight) h = (int)dispHeight - y;
  if (w <= 0 || h <= 0) return;

  const uint8_t* start = display_framebuffer + ((size_t)y * dispWidth + x) * 3;
  const uintptr_t addr = (uintptr_t)start;
  const uintptr_t base = (uintptr_t)display_framebuffer;
  const uintptr_t aligned = addr & ~(uintptr_t)(DISP_CACHE_LINE - 1);
  size_t bytes = (size_t)(addr - aligned) + (size_t)w * h * 3;
  bytes = (bytes + DISP_CACHE_LINE - 1) & ~(size_t)(DISP_CACHE_LINE - 1);
  // The rounding must not run off the end of the framebuffer.
  const size_t fbBytes = (size_t)dispWidth * dispHeight * 3;
  const size_t used = (size_t)(aligned - base) + bytes;
  if (used > fbBytes) bytes = fbBytes - (size_t)(aligned - base);
  if (bytes == 0) return;

  const uint32_t flags = invalidate ? (ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE)
                                    : (ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  esp_cache_msync((void*)aligned, bytes, flags | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
}

// ============================================================
// The interface
// ============================================================

// The colour of a preset button, the same vocabulary the F1's pads use:
// green for the one that is running, blue for a saved one, red for a
// one-shot playlist, magenta for a looping one, dark for nothing.
static void dispPresetColour(uint8_t slot, uint8_t& r, uint8_t& g, uint8_t& b) {
  r = g = b = 0;
  if (!slot || slot > 32) return;
  if (!getCachedPresetExists(slot)) return;
  if (slot == currentPreset) { g = 0x40; return; }   // green: it is running
  b = 0x30;                                          // blue: it is saved
}

// The bar: the board's name, what the display is, and the beat.
static void dispDrawBar(void) {
  const disp_layout_t& L = dispLayout;
  if (!L.barW || !L.barH) return;

  dispFillRect(L.barX, L.barY, L.barW, L.barH, 0x10, 0x10, 0x18);

  const int textY = L.barY + (L.barH - DISP_FONT_H) / 2;
  int x = L.barX + 6;

  // The board's name.
  dispText(x, textY, serverDescription, 0xC0, 0xC0, 0xC0);
  x += dispTextWidth(serverDescription) + 12;

  // What the display is showing.
  if (dispName && dispName[0]) {
    dispText(x, textY, dispName, dispPresent ? 0x40 : 0x60, dispPresent ? 0xC0 : 0x60,
              dispPresent ? 0x40 : 0x60);
  }

  // The beat, at the right: the BPM and the four boxes.
  const ArBeatSource b = arGetBeatSource(false);
  char bpm[16];
  if (b.live) snprintf(bpm, sizeof(bpm), "%.1f", (double)b.bpm);
  else        strcpy(bpm, "--.-");

  const int bpmW = dispTextWidth(bpm);
  int bx = L.barX + L.barW - 6 - bpmW;
  dispText(bx, textY, bpm, 0xE0, 0xE0, 0xE0);

  // The beat boxes, to the left of the BPM.
  const int boxSize = DISP_FONT_H;
  const int boxGap = 3;
  const int boxesW = 4 * boxSize + 3 * boxGap;
  int px = bx - 8 - boxesW;
  for (uint8_t i = 0; i < 4; i++) {
    const bool on = (b.live && b.beatInBar == (i + 1));
    const uint8_t fill = on ? 0x30 : 0x08;
    dispFillRect(px, textY, boxSize, boxSize, fill, fill, on ? 0x60 : 0x20);
    // The downbeat gets a brighter edge.
    if (b.downbeatKnown && i == 0) {
      dispFillRect(px, textY, boxSize, 1, 0xE0, 0xE0, 0xE0);
    }
    px += boxSize + boxGap;
  }
}

// The grid of preset buttons.
static void dispDrawGrid(void) {
  const disp_layout_t& L = dispLayout;
  if (!L.gridW || !L.gridH) return;

  dispFillRect(L.gridX, L.gridY, L.gridW, L.gridH, 0x08, 0x08, 0x0C);

  const uint8_t buttons = dispButtons ? dispButtons : 32;
  for (uint8_t i = 0; i < buttons; i++) {
    const int col = i % L.cols;
    const int row = i / L.cols;
    const int bx = L.gridX + L.btnPad + col * (L.btnW + L.btnPad);
    const int by = L.gridY + L.btnPad + row * (L.btnH + L.btnPad);
    uint8_t r, g, b;
    dispPresetColour((uint8_t)(i + 1), r, g, b);
    dispFillRect(bx, by, L.btnW, L.btnH, r, g, b);
    // The number, when there is room for it.
    if (L.btnW >= 16 && L.btnH >= DISP_FONT_H + 4) {
      char n[4];
      snprintf(n, sizeof(n), "%u", (unsigned)(i + 1));
      dispText(bx + (L.btnW - dispTextWidth(n)) / 2,
               by + (L.btnH - DISP_FONT_H) / 2, n,
               (r || g || b) ? 0xE0 : 0x50,
               (r || g || b) ? 0xE0 : 0x50,
               (r || g || b) ? 0xE0 : 0x50);
    }
  }
}

// The mouse's cursor.
//
// A small crosshair, drawn with a dark outline under it so it can be seen
// on a light picture as well as on a dark one.  Without it there is no
// telling where the pointer is.
static void dispDrawCursor(void) {
  if (!dispPtrSeen || dispPtrX < 0 || dispPtrY < 0) return;
  const int x = dispPtrX, y = dispPtrY;
  const int arm = 6;
  for (int i = -arm; i <= arm; i++) {
    // The dark line first, one pixel out, then the bright one on top.
    dispPixel(x + i, y - 1, 0, 0, 0);
    dispPixel(x + i, y + 1, 0, 0, 0);
    dispPixel(x - 1, y + i, 0, 0, 0);
    dispPixel(x + 1, y + i, 0, 0, 0);
    dispPixel(x + i, y, 0xF0, 0xF0, 0xF0);
    dispPixel(x, y + i, 0xF0, 0xF0, 0xF0);
  }
  // The hot pixel in the middle.
  dispPixel(x, y, dispPtrDown ? 0xFF : 0x30, dispPtrDown ? 0x40 : 0xC0, dispPtrDown ? 0x40 : 0xF0);
}

// The interface, and the cache around it.
//
// The bar and the grid are the CPU's own: the PPA never writes there.  The
// cursor can be anywhere, including on top of the picture, so its lines
// are dropped first - and then put back, because the DMA that presents the
// frame reads the memory and not the cache.
static void dispDrawInterface(void) {
  const disp_layout_t& L = dispLayout;
  const bool iface = (dispView == DISPLAY_VIEW_INTERFACE);

  // Drop the CPU's copy of the regions it is about to write.
  if (iface) {
    dispCacheRegion(L.barX, L.barY, L.barW, L.barH, true);
    dispCacheRegion(L.gridX, L.gridY, L.gridW, L.gridH, true);
  }
  if (dispPtrSeen) dispCacheRegion(dispPtrX - 8, dispPtrY - 8, 17, 17, true);

  if (iface) {
    dispDrawBar();
    dispDrawGrid();
  }
  // The cursor is drawn over everything, in either view.
  dispDrawCursor();

  // And now push it out, so the panel sees it.
  if (iface) {
    dispCacheRegion(L.barX, L.barY, L.barW, L.barH, false);
    dispCacheRegion(L.gridX, L.gridY, L.gridW, L.gridH, false);
  }
  if (dispPtrSeen) dispCacheRegion(dispPtrX - 8, dispPtrY - 8, 17, 17, false);
}

// ============================================================
// The picture
// ============================================================

// The rectangle the picture goes in: the whole frame, or the band between
// the bar and the grid.
static void dispPreviewRect(display_rect_t& r) {
  const disp_layout_t& L = dispLayout;
  if (dispView == DISPLAY_VIEW_INTERFACE && L.prevW > 0 && L.prevH > 0) {
    r.x = L.prevX; r.y = L.prevY; r.w = L.prevW; r.h = L.prevH;
  } else {
    r.x = 0; r.y = 0; r.w = dispWidth; r.h = dispHeight;
  }
}

// The picture's geometry: what is scaled, from where, and where it lands.
struct disp_scale_t {
  float scale;
  int   blockW, blockH;      // the part of the source that is taken
  int   blockOffX, blockOffY;
  int   outW, outH;          // the size of the scaled picture
  int   outX, outY;          // where it lands in the frame
};

// The geometry of the picture.
//
// One PPA pass, always.  The scale is what fits the destination, clamped to
// what one pass can do; the source is cropped in the middle if the result
// would not fit, and the result is centred in the destination.
static bool dispComputeScale(int srcW, int srcH, const display_rect_t& dst, disp_scale_t& g) {
  if (srcW <= 0 || srcH <= 0 || dst.w <= 0 || dst.h <= 0) return false;

  // The rotation: a quarter turn swaps the picture's width and its height.
  //
  // The SOURCE is not transposed: the block is taken out of the source as
  // it is, scaled, and THEN turned.  So the source's width ends up in the
  // output's HEIGHT - and the room the source has to fill is swapped with
  // it.  (Getting this wrong is what makes the PPA say the block does not
  // fit in the picture.)
  const bool quarter = (dispRotation == 90 || dispRotation == 270);
  const int roomW = quarter ? dst.h : dst.w;   // the room for the source's WIDTH
  const int roomH = quarter ? dst.w : dst.h;   // the room for the source's HEIGHT

  float scale = (float)roomW / (float)srcW;
  const float byH = (float)roomH / (float)srcH;
  if (byH < scale) scale = byH;

  // One pass, and the scale is clamped to what the PPA can do in one: from
  // a sixteenth to sixteen times.  Past that the picture is letterboxed
  // (too small) or cropped (too big) - never scaled twice.
  const float PPA_MIN_SCALE = 0.0625f;   // 1/16
  const float PPA_MAX_SCALE = 16.0f;
  if (scale < PPA_MIN_SCALE) scale = PPA_MIN_SCALE;
  if (scale > PPA_MAX_SCALE) scale = PPA_MAX_SCALE;

  // The part of the source the picture is taken from.  If the scaled result
  // would not fit, the source is cropped in the middle instead.
  int blockW = srcW, blockH = srcH;
  int maxBlockW = (int)((float)roomW / scale + 0.5f);
  int maxBlockH = (int)((float)roomH / scale + 0.5f);
  if (maxBlockW < 1) maxBlockW = 1;
  if (maxBlockH < 1) maxBlockH = 1;
  if (blockW > maxBlockW) blockW = maxBlockW;
  if (blockH > maxBlockH) blockH = maxBlockH;

  // The scaled size.  A quarter turn turns the scaled picture, so its
  // width and its height are swapped on the way out.
  int outW, outH;
  if (quarter) {
    outW = (int)((float)blockH * scale + 0.5f);
    outH = (int)((float)blockW * scale + 0.5f);
  } else {
    outW = (int)((float)blockW * scale + 0.5f);
    outH = (int)((float)blockH * scale + 0.5f);
  }
  if (outW < 1) outW = 1;
  if (outH < 1) outH = 1;
  if (outW > dst.w) outW = dst.w;
  if (outH > dst.h) outH = dst.h;

  g.scale      = scale;
  g.blockW     = blockW;
  g.blockH     = blockH;
  g.blockOffX  = (srcW - blockW) / 2;
  g.blockOffY  = (srcH - blockH) / 2;
  g.outW       = outW;
  g.outH       = outH;
  g.outX       = dst.x + (dst.w - outW) / 2;
  g.outY       = dst.y + (dst.h - outH) / 2;
  return true;
}

// The picture, scaled and centred, in one PPA pass.
static bool dispScalePicture(uint8_t* src, int srcW, int srcH, const disp_scale_t& g) {
  if (!src) return false;

  ppa_srm_oper_config_t srm = {};
  srm.in.srm_cm  = PPA_SRM_COLOR_MODE_RGB888;
  srm.out.srm_cm = PPA_SRM_COLOR_MODE_RGB888;
  srm.in.buffer  = src;
  srm.in.pic_w   = srcW;
  srm.in.pic_h   = srcH;
  srm.in.block_w = g.blockW;
  srm.in.block_h = g.blockH;
  srm.in.block_offset_x = g.blockOffX;
  srm.in.block_offset_y = g.blockOffY;

  srm.out.buffer      = display_framebuffer;
  srm.out.buffer_size = (uint32_t)((size_t)dispWidth * dispHeight * 3);
  srm.out.pic_w       = dispWidth;
  srm.out.pic_h       = dispHeight;
  srm.out.block_offset_x = g.outX;
  srm.out.block_offset_y = g.outY;

  srm.scale_x = g.scale;
  srm.scale_y = g.scale;
  switch (dispRotation) {
    case 90:  srm.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;  break;
    case 180: srm.rotation_angle = PPA_SRM_ROTATION_ANGLE_180; break;
    case 270: srm.rotation_angle = PPA_SRM_ROTATION_ANGLE_270; break;
    default:  srm.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;   break;
  }
  srm.rgb_swap  = 0;      // the framebuffer is RGB, like everything else
  srm.byte_swap = 0;
  srm.mode      = PPA_TRANS_MODE_BLOCKING;

  const esp_err_t err = ppa_do_scale_rotate_mirror(ppa_srm_handle, &srm);
  if (err != ESP_OK) {
    // Say what was asked for: the PPA's own message says what was wrong,
    // but not what we asked for.
    USER_PRINTF("Display: the PPA refused the picture: in %ux%u block %ux%u at %u,%u"
                " out %ux%u at %u,%u scale %f\n",
                srm.in.pic_w, srm.in.pic_h, srm.in.block_w, srm.in.block_h,
                srm.in.block_offset_x, srm.in.block_offset_y,
                srm.out.pic_w, srm.out.pic_h, srm.out.block_offset_x, srm.out.block_offset_y,
                (double)srm.scale_x);
    return false;
  }
  return true;
}

// ============================================================
// The blit: the picture, the interface, and the flip
// ============================================================

static uint32_t dispBlitOk = 0, dispBlitFail = 0;

// Where the picture landed last time - so the margins around it are only
// cleared when it moves.
//
// There are TWO buffers, and they take turns being written, so each one
// has to remember its own picture: a buffer that has not been cleared yet
// still has the last picture's margins in it, and they would show up on
// the screen when the buffers swap.
static int dispBufParity = 0;
static int dispLastOut[2][4] = {{-1,-1,-1,-1},{-1,-1,-1,-1}};   // x, y, w, h

static void dispBlit(void) {
  uint8_t* back = display_framebuffer;
  esp_lcd_panel_handle_t ph = panel_handle;
  if (!back || !ph || !dispWidth || !dispHeight) return;

  static uint32_t lastBlitUs = 0;
  const uint32_t nowUs = esp_timer_get_time();
  if (nowUs - lastBlitUs < 33333) return;   // ~30 fps
  if (xSemaphoreTake(busMutex, 0) != pdTRUE) return;
  lastBlitUs = nowUs;

  Bus* bus = busses.getBus(0);
  uint8_t* busPixelData = bus ? bus->getPixelData() : nullptr;

  if (busPixelData) {
    // The picture the bus really holds: the matrix can be taller than the
    // LEDs fill, and the PPA refuses a picture that does not fit.
    const uint16_t matW = Segment::maxWidth;
    const uint16_t matH = Segment::maxHeight;
    const ppa_bus_picture_t pic = ppaBusPicture(bus, matW, matW, matH);
    display_rect_t dst;
    dispPreviewRect(dst);

    disp_scale_t g;
    if (dispComputeScale(pic.w, pic.h, dst, g)) {
      // The margins around the picture are black.  They only have to be
      // cleared when the picture moves or changes size - which is when
      // the matrix changes or the picture is turned, not every frame.
      // Each buffer remembers its own picture, because they take turns.
      const int par = dispBufParity & 1;
      if (dispLastOut[par][0] != g.outX || dispLastOut[par][1] != g.outY ||
          dispLastOut[par][2] != g.outW || dispLastOut[par][3] != g.outH) {
        dispCacheRegion(dst.x, dst.y, dst.w, dst.h, true);
        // Above the picture.
        dispFillRect(dst.x, dst.y, dst.w, g.outY - dst.y, 0, 0, 0);
        // Below it.
        dispFillRect(dst.x, g.outY + g.outH, dst.w, dst.y + dst.h - (g.outY + g.outH), 0, 0, 0);
        // Left of it.
        dispFillRect(dst.x, g.outY, g.outX - dst.x, g.outH, 0, 0, 0);
        // Right of it.
        dispFillRect(g.outX + g.outW, g.outY, dst.x + dst.w - (g.outX + g.outW), g.outH, 0, 0, 0);
        // And push the margins out: the DMA presents the memory, not the
        // cache.  The picture's own lines were dropped above, so they are
        // not written back - only the margins' are.
        dispCacheRegion(dst.x, dst.y, dst.w, dst.h, false);
        dispLastOut[par][0] = g.outX; dispLastOut[par][1] = g.outY;
        dispLastOut[par][2] = g.outW; dispLastOut[par][3] = g.outH;
      }

      if (dispScalePicture(busPixelData, pic.w, pic.h, g)) {
        dispBlitOk++;
        // The interface goes on top of the picture.
        dispDrawInterface();
        // Present the back buffer at the next vsync, and swap.
        esp_lcd_panel_draw_bitmap(ph, 0, 0, dispWidth, dispHeight, back);
        uint8_t* tmp = display_front_framebuffer;
        display_front_framebuffer = back;
        display_framebuffer = tmp;
        dispBufParity++;
      } else {
        dispBlitFail++;
      }
    }
  }

  xSemaphoreGive(busMutex);
}

// ============================================================
// The touch
// ============================================================

static uint16_t dispTouchX = 0, dispTouchY = 0;
static uint16_t dispTouchS = 0;
static bool     dispTouchDown = false;

// What is at a point: 0 = nothing, otherwise the button number (1-based).
static uint8_t dispHitTest(int x, int y) {
  const disp_layout_t& L = dispLayout;
  if (dispView != DISPLAY_VIEW_INTERFACE) return 0;
  if (x < L.gridX || x >= L.gridX + L.gridW) return 0;
  if (y < L.gridY || y >= L.gridY + L.gridH) return 0;

  const int lx = x - L.gridX - L.btnPad;
  const int ly = y - L.gridY - L.btnPad;
  if (lx < 0 || ly < 0) return 0;
  const int block = L.btnW + L.btnPad;
  if (block <= 0) return 0;
  const int col = lx / block;
  const int row = ly / block;
  // A press in the gap between two buttons is not a press.
  if ((lx % block) >= L.btnW) return 0;
  if ((ly % (L.btnH + L.btnPad)) >= L.btnH) return 0;
  if (col < 0 || col >= L.cols || row < 0 || row >= L.rows) return 0;

  const uint8_t button = (uint8_t)(row * L.cols + col + 1);
  if (button > dispButtons) return 0;      // an empty cell
  return button;
}

static void dispButtonPress(uint8_t button) {
  if (!button || button > 32) return;
  if (!getCachedPresetExists(button)) return;
  unloadPlaylist();
  applyPreset(button);
  handlePresets();
}

// A point, from a finger or from a mouse.  The same path for both.
static void dispPoint(int x, int y, bool down) {
  if (down) {
    const uint8_t button = dispHitTest(x, y);
    if (button) dispButtonPress(button);
  }
}

static void dispPollTouch(void) {
  if (!dispTouch) return;
  static uint32_t lastPoll = 0;
  const uint32_t now = millis();
  if (now - lastPoll < 20) return;   // 50 Hz is plenty for a finger
  lastPoll = now;

  if (esp_lcd_touch_read_data(dispTouch) != ESP_OK) return;
  uint8_t count = 0;
  // The touch layer says NO when there is no point - and that is the end of
  // a touch, not a failure.  The finger has to be let go here, or the next
  // touch is not a rising edge and never presses anything.
  const bool got = esp_lcd_touch_get_coordinates(dispTouch, &dispTouchX, &dispTouchY, &dispTouchS, &count, 1);
  if (!got || !count) {
    dispTouchDown = false;
    return;
  }

  const bool down = !dispTouchDown;
  if (down) USER_PRINTF("Display: touch at %u,%u\n", dispTouchX, dispTouchY);
  dispTouchDown = true;
  dispPoint(dispTouchX, dispTouchY, down);
}

// ============================================================
// The DSI panel
// ============================================================

static void dispDsiDeinit(void) {
  if (dispTouch)   { esp_lcd_touch_del(dispTouch);   dispTouch = NULL; }
  if (dispTouchIo) { esp_lcd_panel_io_del(dispTouchIo); dispTouchIo = NULL; }
  if (panel_handle) { esp_lcd_panel_del(panel_handle); panel_handle = NULL; }
  if (dispDbiIo)   { esp_lcd_panel_io_del(dispDbiIo); dispDbiIo = NULL; }
  if (dispDsiBus)  { esp_lcd_del_dsi_bus(dispDsiBus);  dispDsiBus = NULL; }
  dispWidth = 0;
  dispHeight = 0;
}

static bool dispDsiInit(const dsi_panel_t& p) {
  // The DSI bus.
  esp_lcd_dsi_bus_config_t busCfg = {};
  busCfg.bus_id              = 0;
  busCfg.num_data_lanes      = p.lanes;
  busCfg.phy_clk_src         = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
  busCfg.lane_bit_rate_mbps  = p.lane_mbps;
  USER_PRINTF("Display: MIPI DSI bus, %u lane(s) at %u Mbps\n", p.lanes, p.lane_mbps);
  if (esp_lcd_new_dsi_bus(&busCfg, &dispDsiBus) != ESP_OK) {
    USER_PRINTLN("Display: the DSI bus would not start");
    return false;
  }

  // The DPI side: the pixel clock and the timings.
  esp_lcd_dpi_panel_config_t dpi = {};
  dpi.dpi_clk_src            = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
  dpi.dpi_clock_freq_mhz     = p.dpi_mhz;
  dpi.virtual_channel        = 0;
  dpi.pixel_format           = LCD_COLOR_PIXEL_FORMAT_RGB888;
  dpi.in_color_format        = LCD_COLOR_FMT_RGB888;
  dpi.out_color_format       = LCD_COLOR_FMT_RGB888;
  dpi.num_fbs                = 2;
  dpi.video_timing.h_size            = p.width;
  dpi.video_timing.v_size            = p.height;
  dpi.video_timing.hsync_back_porch  = p.hbp;
  dpi.video_timing.hsync_pulse_width = p.hsw;
  dpi.video_timing.hsync_front_porch = p.hfp;
  dpi.video_timing.vsync_back_porch  = p.vbp;
  dpi.video_timing.vsync_pulse_width = p.vsw;
  dpi.video_timing.vsync_front_porch = p.vfp;
  dpi.flags.use_dma2d        = true;
  dpi.flags.disable_lp       = true;

  // The DBI channel the panel's own commands go out on.
  esp_lcd_dbi_io_config_t dbi = {};
  dbi.virtual_channel = 0;
  dbi.lcd_cmd_bits    = 8;
  dbi.lcd_param_bits  = 8;
  if (esp_lcd_new_panel_io_dbi(dispDsiBus, &dbi, &dispDbiIo) != ESP_OK) {
    USER_PRINTLN("Display: the DBI channel would not start");
    return false;
  }

  const esp_lcd_panel_dev_config_t panelCfg = {
    .reset_gpio_num = (displayConfig.reset_gpio >= 0) ? displayConfig.reset_gpio : p.reset_gpio,
    .rgb_ele_order  = dispEleBgr ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
    .bits_per_pixel = 24,
    .vendor_config  = nullptr,
  };

  bool ok = false;
  switch (p.driver) {
    case DRV_ILI9881C: {
      ili9881c_vendor_config_t vc = {};
      vc.mipi_config.dsi_bus    = dispDsiBus;
      vc.mipi_config.dpi_config = &dpi;
      vc.mipi_config.lane_num   = p.lanes;
      esp_lcd_panel_dev_config_t cfg = panelCfg;
      cfg.vendor_config = &vc;
      ok = (esp_lcd_new_panel_ili9881c(dispDbiIo, &cfg, &panel_handle) == ESP_OK);
      break;
    }
    case DRV_EK79007: {
      ek79007_vendor_config_t vc = {};
      vc.mipi_config.dsi_bus    = dispDsiBus;
      vc.mipi_config.dpi_config = &dpi;
      vc.mipi_config.lane_num   = p.lanes;
      esp_lcd_panel_dev_config_t cfg = panelCfg;
      cfg.vendor_config = &vc;
      ok = (esp_lcd_new_panel_ek79007(dispDbiIo, &cfg, &panel_handle) == ESP_OK);
      break;
    }
    case DRV_ST7703: {
      st7703_vendor_config_t vc = {};
      vc.mipi_config.dsi_bus    = dispDsiBus;
      vc.mipi_config.dpi_config = &dpi;
      vc.flags.use_mipi_interface = 1;
      esp_lcd_panel_dev_config_t cfg = panelCfg;
      cfg.vendor_config = &vc;
      ok = (esp_lcd_new_panel_st7703(dispDbiIo, &cfg, &panel_handle) == ESP_OK);
      break;
    }
    case DRV_JD9365: {
      jd9365_vendor_config_t vc = {};
      vc.mipi_config.dsi_bus    = dispDsiBus;
      vc.mipi_config.dpi_config = &dpi;
      vc.mipi_config.lane_num   = p.lanes;
      vc.flags.use_mipi_interface = 1;
      esp_lcd_panel_dev_config_t cfg = panelCfg;
      cfg.vendor_config = &vc;
      ok = (esp_lcd_new_panel_jd9365(dispDbiIo, &cfg, &panel_handle) == ESP_OK);
      break;
    }
    default:
      break;
  }
  if (!ok || !panel_handle) {
    USER_PRINTLN("Display: the panel driver refused the panel");
    return false;
  }

  if (esp_lcd_panel_reset(panel_handle) != ESP_OK) return false;
  if (esp_lcd_panel_init(panel_handle) != ESP_OK) {
    USER_PRINTLN("Display: the panel would not start");
    return false;
  }
  esp_lcd_panel_disp_on_off(panel_handle, true);

  // The framebuffers.
  void* fb0 = NULL; void* fb1 = NULL;
  if (esp_lcd_dpi_panel_get_frame_buffer(panel_handle, 2, &fb0, &fb1) != ESP_OK || !fb0 || !fb1) {
    USER_PRINTLN("Display: no framebuffers");
    return false;
  }
  const size_t fbBytes = (size_t)p.width * p.height * 3;
  memset(fb0, 0, fbBytes);
  memset(fb1, 0, fbBytes);
  display_front_framebuffer = (uint8_t*)fb0;
  display_framebuffer = (uint8_t*)fb1;
  dispWidth  = p.width;
  dispHeight = p.height;

  USER_PRINTF("Display: %s is up, %ux%u at %u MHz\n", p.name, p.width, p.height, p.dpi_mhz);

  // The backlight.  Some panels have it the other way round: the pin is LOW
  // when the light is on.
  const int8_t bl = (displayConfig.backlight_gpio >= 0) ? displayConfig.backlight_gpio
                                                        : p.backlight_gpio;
  if (bl >= 0 && (p.flags & DSI_FLAG_BACKLIGHT)) {
    const bool activeLow = (p.flags & DSI_FLAG_BACKLIGHT_LO) != 0;
    pinMode(bl, OUTPUT);
    digitalWrite(bl, activeLow ? LOW : HIGH);
  }

  // The touch.
  if (p.touch == TCH_GT911 && (p.flags & DSI_FLAG_TOUCH)) {
    i2c_master_bus_handle_t i2c = globalI2cBusEnsure((int8_t)HW_PIN_SDA, (int8_t)HW_PIN_SCL);
    if (i2c) {
      esp_lcd_panel_io_i2c_config_t tio = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
      tio.dev_addr    = p.touch_addr;
      tio.scl_speed_hz = 400000;
      esp_lcd_touch_io_gt911_config_t tcfg = { .dev_addr = p.touch_addr };
      esp_lcd_touch_config_t tconf = {};
      tconf.x_max = p.touch_x_max;
      tconf.y_max = p.touch_y_max;
      tconf.rst_gpio_num = GPIO_NUM_NC;
      tconf.int_gpio_num = GPIO_NUM_NC;
      tconf.driver_data = &tcfg;
      if (esp_lcd_new_panel_io_i2c(i2c, &tio, &dispTouchIo) == ESP_OK &&
          esp_lcd_touch_new_i2c_gt911(dispTouchIo, &tconf, &dispTouch) == ESP_OK) {
        USER_PRINTF("Display: the touch controller is up (0x%02X)\n", p.touch_addr);
      } else {
        dispTouch = NULL;
        USER_PRINTLN("Display: no touch controller");
      }
    }
  }
  return true;
}

// ============================================================
// What is on the bus
// ============================================================

// A touch controller on the bus means a DSI panel is attached: every panel
// in the table has one.
bool displayTouchControllerPresent(void) {
  static const uint8_t addrs[] = { 0x5D, 0x14, 0x38, 0x15 };
  static int answered = -1;
  if (answered >= 0) return answered == 1;
  answered = 0;
  i2c_master_bus_handle_t bus = globalI2cBusEnsure((int8_t)HW_PIN_SDA, (int8_t)HW_PIN_SCL);
  if (!bus) return false;
  for (uint8_t i = 0; i < sizeof(addrs); i++) {
    if (i2c_master_probe(bus, addrs[i], 20) == ESP_OK) {
      answered = 1;
      USER_PRINTF("Display: something with a touch is on the bus (0x%02X)\n", addrs[i]);
      break;
    }
  }
  return answered == 1;
}

// Does the display need the MIPI PHY's rail (LDO channel 3 at 2.5 V)?
// The bridge needs it, and so does a DSI panel - and the rail is the same
// channel the high GPIOs would use at 3.3 V, so the two cannot both have it.
bool displayNeedsLdo(void) {
  if (hdmiBridgePresent()) return true;
  return displayTouchControllerPresent();
}

// ============================================================
// Bringing the display up
// ============================================================

static void dispTakeLdo(void) {
  static bool taken = false;
  if (taken) return;
  esp_ldo_channel_config_t cfg = { .chan_id = 3, .voltage_mv = 2500 };
  esp_ldo_channel_handle_t ldo = NULL;
  if (esp_ldo_acquire_channel(&cfg, &ldo) == ESP_OK && ldo) {
    taken = true;
    USER_PRINTLN("Display: the MIPI DSI PHY is powered (LDO channel 3 at 2.5V)");
  } else {
    USER_PRINTLN("Display: could not take the MIPI DSI PHY's rail - the display may not work");
  }
}

static void dispTearDown(void) {
  if (dispKind == DISPLAY_HDMI) {
    hdmiBridgeDeinit();
  } else if (dispKind == DISPLAY_DSI) {
    dispDsiDeinit();
  }
  dispKind = DISPLAY_NONE;
  dispPanel = nullptr;
  dispName = "";
  dispWidth = 0;
  dispHeight = 0;
  dispPresent = false;
  dispLayoutCalc();
}

// The output the board should use: what the settings page says, or what the
// bus answers.
static void dispPickOutput(void) {
  dispKind = DISPLAY_NONE;
  dispPanel = nullptr;

  // The bridge first: it is the one that can be recognised for sure.
  if (hdmiBridgePresent()) {
    dispKind = DISPLAY_HDMI;
    return;
  }

  // A DSI panel: the one the settings page names, if it names one.
  const dsi_panel_t* p = dsiPanelByName(displayConfig.panel);
  if (p) {
    dispKind = DISPLAY_DSI;
    dispPanel = p;
    return;
  }

  // Nothing was chosen and nothing was found.
  dispKind = DISPLAY_NONE;
}

static bool dispBringUp(void) {
  dispTearDown();

  dispView      = displayConfig.view;
  dispButtons   = displayConfig.buttons ? displayConfig.buttons : 32;
  dispButtonCols= displayConfig.button_cols ? displayConfig.button_cols : 8;
  dispRotation  = displayConfig.rotation;

  dispPickOutput();

  if (dispKind == DISPLAY_NONE) {
    USER_PRINTLN("Display: nothing is attached - the display stays off");
    return false;
  }

  dispTakeLdo();

  if (dispKind == DISPLAY_HDMI) {
    // The mode: the one that was chosen, or the one the monitor asks for.
    int mode = hdmiBridgeModeIndex(displayConfig.hdmi_mode);
    if (mode < 0) mode = 0;
    if (!hdmiBridgeInit(mode)) {
      dispKind = DISPLAY_NONE;
      return false;
    }
    dispWidth  = hdmiBridgeWidth();
    dispHeight = hdmiBridgeHeight();
    dispName   = hdmiBridgeModeName(hdmiBridgeCurrentMode());
    dispEleBgr = 0;
  } else {
    dispEleBgr = dispPanel->ele_bgr;
    if (!dispDsiInit(*dispPanel)) {
      dispTearDown();
      return false;
    }
    dispName = dispPanel->name;
  }

  dispLayoutCalc();
  dispPresent = true;
  return true;
}

// ============================================================
// The public interface
// ============================================================

void displaySetup(void) {
  // The settings come out of cfg.json, but the defaults have to be there
  // before it is read.
  displayConfig.view        = DISPLAY_VIEW_ONLY;
  displayConfig.rotation    = 0;
  displayConfig.auto_detect = 1;
  displayConfig.buttons     = 32;
  displayConfig.button_cols = 8;
  displayConfig.reset_gpio  = -1;
  displayConfig.backlight_gpio = -1;
  displayConfig.panel[0]    = '\0';
  // The bridge's default mode: the one the board used before the mode became
  // a setting.  The monitor's own EDID still wins over it.
  strlcpy(displayConfig.hdmi_mode, "P4_1280x720_60HZ", sizeof(displayConfig.hdmi_mode));

  dispLayoutCalc();
}

// Bring the output up.  This is called once the settings have been read, and
// it is slow: the bridge waits up to five seconds for the monitor.
void displayStart(void) {
  dispView      = displayConfig.view;
  dispButtons   = displayConfig.buttons ? displayConfig.buttons : 32;
  dispButtonCols= displayConfig.button_cols ? displayConfig.button_cols : 8;
  dispRotation  = displayConfig.rotation;
  dispBringUp();
}

void displayLoop(void) {
  if (dispPendingChange) {
    dispPendingChange = false;
    // The panel is torn down and brought up again, which takes seconds on
    // the bridge (it waits for the monitor).  That is why it happens here
    // and not in the web request that asked for it.
    USER_PRINTF("Display: switching to %s\n",
                displayConfig.panel[0] ? displayConfig.panel : displayConfig.hdmi_mode);
    dispBringUp();
  }

  if (dispKind == DISPLAY_HDMI) hdmiBridgePollLink();

  if (!dispPresent) return;

  dispPollTouch();
  dispBlit();

  // A line every five seconds: what the display is doing.  Quiet when it
  // is all well - unless the picture is not getting through.
  static uint32_t logMs = 0;
  const uint32_t now = millis();
  if (now - logMs >= 5000) {
    logMs = now;
    if (dispBlitFail) {
      USER_PRINTF("Display: %s %ux%u, blit ok=%u fail=%u\n",
                  dispName ? dispName : "?", dispWidth, dispHeight, dispBlitOk, dispBlitFail);
      dispBlitOk = dispBlitFail = 0;
    }
  }
}

void displayReconfigure(void) {
  dispPendingChange = true;
}

bool displayReconfigurePending(void) {
  return dispPendingChange;
}

void displaySetPanel(const char* name) {
  strlcpy(displayConfig.panel, name ? name : "", sizeof(displayConfig.panel));
  dispPendingChange = true;
}

void displaySetHdmiMode(const char* name) {
  strlcpy(displayConfig.hdmi_mode, name ? name : "", sizeof(displayConfig.hdmi_mode));
  dispPendingChange = true;
}

void displaySetView(uint8_t view) {
  displayConfig.view = view;
  dispView = view;
  dispLayoutCalc();
}

void displaySetInterface(bool on) {
  displaySetView(on ? DISPLAY_VIEW_INTERFACE : DISPLAY_VIEW_ONLY);
}

void displaySetRotation(uint8_t degrees) {
  switch (degrees) {
    case 90: case 180: case 270: break;
    default: degrees = 0;
  }
  displayConfig.rotation = degrees;
  dispRotation = degrees;
}

void displayRotateStep(void) {
  uint8_t next = dispRotation + 90;
  if (next >= 360) next = 0;
  displaySetRotation(next);
}

void displaySetButtons(uint8_t count, uint8_t cols) {
  displayConfig.buttons = count;
  displayConfig.button_cols = cols;
  dispButtons = count ? count : 32;
  dispButtonCols = cols ? cols : 8;
  dispLayoutCalc();
}

void displayPointerMove(int x, int y) {
  dispPtrX = x; dispPtrY = y;
  dispPtrSeen = true;
  if (dispPtrDown) dispPoint(x, y, false);
}

void displayPointerButton(bool down) {
  dispPtrDown = down;
  if (down && dispPtrX >= 0 && dispPtrY >= 0) dispPoint(dispPtrX, dispPtrY, true);
}

void displayPointerRelease(void) {
  dispPtrDown = false;
  dispPtrX = -1; dispPtrY = -1;
}

void displayGetStatus(display_status_t& out) {
  out.kind = dispKind;
  out.name = dispName ? dispName : "";
  out.width = dispWidth;
  out.height = dispHeight;
  out.present = dispPresent;
  out.interface_shown = (dispView == DISPLAY_VIEW_INTERFACE);
  out.rotation = dispRotation;
  out.can_rotate = (dispKind == DISPLAY_DSI);
  out.buttons = dispButtons;
  out.button_cols = dispButtonCols;
}

void displayStatusJson(JsonObject& obj) {
  display_status_t s;
  displayGetStatus(s);
  const char* kind = (s.kind == DISPLAY_HDMI) ? "HDMI"
                   : (s.kind == DISPLAY_DSI)  ? "DSI" : "NONE";
  // The short name: the panel's first word ("EK79007"), or the size of the
  // monitor behind the bridge.
  char shortName[16] = "";
  if (s.kind == DISPLAY_HDMI) {
    snprintf(shortName, sizeof(shortName), "%ux%u", s.width, s.height);
  } else if (s.name && s.name[0]) {
    strlcpy(shortName, s.name, sizeof(shortName));
    char* sp = strchr(shortName, ' ');
    if (sp) *sp = '\0';
  }
  obj["kind"] = kind;
  obj["name"] = s.name;
  obj["s"] = shortName;
  obj["w"] = s.width;
  obj["h"] = s.height;
  obj["ok"] = s.present;
  obj["iface"] = s.interface_shown;
  obj["rot"] = s.rotation;
  obj["rr"] = s.can_rotate;
  obj["buttons"] = s.buttons;
  obj["cols"] = s.button_cols;
  // Where the finger is.  Useful when a touch panel is not behaving: it
  // says whether the controller is answering at all, and whether it
  // answers with coordinates that make sense.
  obj["tx"] = dispTouchX;
  obj["ty"] = dispTouchY;
  obj["td"] = dispTouchDown;
  obj["th"] = (dispTouch != NULL);
}

uint16_t displayWidth(void)  { return dispWidth; }
uint16_t displayHeight(void) { return dispHeight; }

#endif // CONFIG_IDF_TARGET_ESP32P4

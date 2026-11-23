#include "wled_screenshot.h"

#if WLED_HAS_SCREENSHOT

#include "wled.h"
#include <esp_heap_caps.h>
#include <esp_cache.h>
#include "driver/jpeg_encode.h"

#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include "wled_display.h"
#endif

// ONE SCREENSHOT AT A TIME.
//
// The encoder engine and its working memory are shared, and two requests
// in it at the same time give a picture that is half of one and half of
// the other.  The lock is held over the whole grab-and-encode.
static SemaphoreHandle_t shotLock = nullptr;

static bool shotLockTake(void) {
  if (!shotLock) shotLock = xSemaphoreCreateMutex();
  if (!shotLock) return false;
  return xSemaphoreTake(shotLock, pdMS_TO_TICKS(5000)) == pdTRUE;
}

static void shotLockGive(void) {
  if (shotLock) xSemaphoreGive(shotLock);
}

// THE BUFFERS.
//
// They are in PSRAM: the internal heap is nearly full on this board, and
// a 720p picture is 2.7 MB.  The encoder's DMA2D reads the picture, so
// the buffer has to be DMA-able, and the CPU's cache has to be told
// about it: the DMA does not see the cache, so what the CPU wrote would
// not be what the DMA reads.  The cache is synced here - the driver does
// it for the buffers it allocates itself, but not for ours.
//
// The cache work wants whole lines: the address and the size have to be
// multiples of the line, or it refuses and says so.  The P4's line is 128
// bytes - the JPEG's length is whatever it happens to be.
#define SHOT_CACHE_LINE 128

static uint8_t* allocShotBuffer(size_t size) {
  // A line of slack at the end: the cache work rounds the size up to whole
  // cache lines, and it must not reach past the buffer.
  const size_t aligned = (size + SHOT_CACHE_LINE * 2) & ~(size_t)(SHOT_CACHE_LINE - 1);
  uint8_t* p = (uint8_t*)heap_caps_aligned_alloc(SHOT_CACHE_LINE, aligned,
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  if (!p) p = (uint8_t*)heap_caps_aligned_alloc(SHOT_CACHE_LINE, aligned,
                          MALLOC_CAP_DMA | MALLOC_CAP_8BIT);   // internal, if there is room
  return p;
}

// The CPU wrote the buffer, the DMA reads it: the cache goes to memory.
static void shotCacheWriteback(void* addr, size_t size) {
  const uintptr_t a = (uintptr_t)addr;
  const uintptr_t aligned = a & ~(uintptr_t)(SHOT_CACHE_LINE - 1);
  size_t bytes = (size_t)(a - aligned) + size;
  bytes = (bytes + SHOT_CACHE_LINE - 1) & ~(size_t)(SHOT_CACHE_LINE - 1);
  esp_cache_msync((void*)aligned, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M
                                | ESP_CACHE_MSYNC_FLAG_INVALIDATE
                                | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
}

// The DMA wrote the buffer, the CPU reads it: the cache goes to the DMA.
static void shotCacheInvalidate(void* addr, size_t size) {
  const uintptr_t a = (uintptr_t)addr;
  const uintptr_t aligned = a & ~(uintptr_t)(SHOT_CACHE_LINE - 1);
  size_t bytes = (size_t)(a - aligned) + size;
  bytes = (bytes + SHOT_CACHE_LINE - 1) & ~(size_t)(SHOT_CACHE_LINE - 1);
  esp_cache_msync((void*)aligned, bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
}

// The encoder engine.  It is made once and kept: the decoder is done the
// same way (jpgd_handle in wled.cpp), and a per-request engine would pay
// for its working memory every time.
static jpeg_encoder_handle_t shotEncoder = nullptr;

static bool shotEncoderGet(jpeg_encoder_handle_t& enc) {
  if (shotEncoder) {
    enc = shotEncoder;
    return true;
  }
  jpeg_encode_engine_cfg_t eng_cfg = {};
  eng_cfg.intr_priority = 0;        // the driver picks
  eng_cfg.timeout_ms  = 1000;       // a 720p picture takes longer than the decoder's 40 ms
  if (jpeg_new_encoder_engine(&eng_cfg, &shotEncoder) != ESP_OK || !shotEncoder) {
    shotEncoder = nullptr;
    USER_PRINTLN("screenshot: the encoder would not start");
    return false;
  }
  enc = shotEncoder;
  return true;
}

// Encodes a picture that is ALREADY in the encoder's input buffer.
//
// The buffer holds the picture in the order the encoder reads it: the
// first byte of a pixel is the BLUE one.  (That is the DMA2D's order -
// the panel's own framebuffer is in it too, which is why the HDMI picture
// can be handed over as it is.)
//
// The height is trimmed if the encoder refuses the picture - the encoder
// works on MCU blocks, so the sides have to be a multiple of 8 (16 for the
// chroma-subsampled modes).  Only the height can be trimmed: the rows are
// contiguous, the columns are not.
// The JPEG's own buffer comes from the SAME allocation as the picture: two
// separate allocations can land on top of each other, and then the picture's
// write-back puts the picture over the JPEG's header.
static bool encodeShotPicture(uint8_t* in, uint16_t w, uint16_t h, uint8_t quality,
                              uint8_t* outBuf, size_t outCap,
                              size_t& outLen, uint16_t& outW, uint16_t& outH) {
  outLen = 0;
  if (!in || !w || !h || !outBuf) return false;
  if (w & 7) {                       // the width cannot be trimmed - give up
    USER_PRINTLN("screenshot: the width is not a multiple of 8");
    return false;
  }

  jpeg_encoder_handle_t enc = nullptr;
  if (!shotEncoderGet(enc)) return false;

  const uint16_t heights[3] = { h, (uint16_t)(h & ~15), (uint16_t)(h & ~7) };
  // The chroma modes, best first: no chroma loss keeps the text sharp, but
  // the encoder may not take it - then 4:2:0 does.
  const jpeg_down_sampling_type_t subs[2] = { JPEG_DOWN_SAMPLING_YUV444, JPEG_DOWN_SAMPLING_YUV420 };

  // The JPEG itself: a tenth of the picture is plenty for a still, and a
  // 720p picture with noise can want more, so the buffer is generous.
  for (uint8_t attempt = 0; attempt < 3; attempt++) {
    const uint16_t hh = heights[attempt];
    if (!hh || hh > h) continue;

    for (uint8_t s = 0; s < 2; s++) {
      jpeg_encode_cfg_t cfg = {};
      cfg.width         = w;
      cfg.height        = hh;
      cfg.src_type      = JPEG_ENCODE_IN_FORMAT_RGB888;
      cfg.sub_sample    = subs[s];
      cfg.image_quality = quality;

      uint8_t* out = outBuf;

      // The CPU filled the picture, the DMA reads it.
      shotCacheWriteback(in, (size_t)w * hh * 3);

      // The CPU's copy of the JPEG's buffer is dropped BEFORE the encoder
      // writes into it.  The encoder writes through the DMA, so a stale
      // line that is written back afterwards lands on the JPEG's header -
      // and the header is what the CPU reads first.
      shotCacheInvalidate(out, outCap);

      uint32_t outSize = 0;
      const esp_err_t err = jpeg_encoder_process(enc, &cfg, in, (uint32_t)((size_t)w * hh * 3),
                                                  out, (uint32_t)outCap, &outSize);
      if (err == ESP_OK && outSize > 0) {
        outLen = outSize;
        outW   = w;
        outH   = hh;
        return true;
      }
      USER_PRINTF("screenshot: the encoder refused %ux%u, chroma %d (err %d)\n",
                  w, hh, (int)subs[s], (int)err);
    }
  }
  return false;
}

// The LED bus buffer: the canvas at its native resolution.
//
// The bus keeps its pixels in its own colour order, so the colours are read
// through getPixelColor() - that is what the LED would actually show.  The
// picture is put into the encoder's order: BLUE first.
static bool grabBusPicture(uint8_t* rgb, uint16_t w, uint16_t h) {
  Bus* bus = busses.getBus(0);
  if (!bus || !bus->isOk()) return false;

  const uint32_t need = (uint32_t)w * h;
  if (bus->getLength() < need) return false;

  // The bus lock, so the render cannot move the pixels under us.
  if (xSemaphoreTake(busMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    USER_PRINTLN("screenshot: the bus is busy");
    return false;
  }
  uint8_t* p = rgb;
  for (uint32_t i = 0; i < need; i++) {
    const uint32_t c = bus->getPixelColor(i);
    *p++ = B(c);
    *p++ = G(c);
    *p++ = R(c);
  }
  xSemaphoreGive(busMutex);
  return true;
}

#if defined(CONFIG_IDF_TARGET_ESP32P4)
// The display's framebuffer: what the screen shows (the canvas, scaled up).
//
// The framebuffer is RGB - the first byte of a pixel is the red one - and
// the encoder wants the opposite order, so the red and the blue are
// swapped on the way in.
static bool grabHdmPicture(uint8_t* rgb, uint16_t w, uint16_t h) {
  // The pointer is taken under the bus lock, so the blit cannot swap the
  // front and the back while the pointer is read.  The picture itself is
  // read outside the lock: the front buffer is not being written (the back
  // one is), so the only risk is a torn frame, which is fine for a look.
  const uint8_t* fb = nullptr;
  if (xSemaphoreTake(busMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    USER_PRINTLN("screenshot: the bus is busy");
    return false;
  }
  fb = display_front_framebuffer;
  xSemaphoreGive(busMutex);
  if (!fb) return false;
  const size_t size = (size_t)w * h * 3;
  // The PPA wrote the framebuffer, so the CPU's cache may still hold an
  // older copy of it.
  shotCacheInvalidate((void*)fb, size);
  uint8_t* p = rgb;
  const uint8_t* s = fb;
  for (size_t i = 0; i < size; i += 3) {
    p[0] = s[2];   // blue first - the encoder's order
    p[1] = s[1];
    p[2] = s[0];
    p += 3; s += 3;
  }
  return true;
}
#endif

// THE RESPONSE.
//
// It streams the JPEG straight out of the buffer the encoder made - the
// buffer lives in PSRAM, and the web server never copies it into the
// internal heap.  When the response is deleted the buffer goes with it.
class ShotJpegResponse : public AsyncAbstractResponse {
  uint8_t* _buf;      // the whole allocation - it is freed as one piece
  size_t   _len;
  size_t   _read;
  size_t   _off;      // where the JPEG starts inside it

public:
  ShotJpegResponse(uint8_t* buf, size_t len, size_t off)
    : _buf(buf), _len(len), _read(0), _off(off) {
    _code = 200;
    _contentType = "image/jpeg";
    _contentLength = len;
    _sendContentLength = true;
  }

  ~ShotJpegResponse() {
    if (_buf) {
      free(_buf);
      _buf = nullptr;
    }
  }

  bool _sourceValid() const override { return true; }

  size_t _fillBuffer(uint8_t* data, size_t len) override {
    const size_t left = _len - _read;
    if (left > len) {
      memcpy(data, _buf + _off + _read, len);
      _read += len;
      return len;
    }
    memcpy(data, _buf + _off + _read, left);
    _read += left;
    return left;
  }
};

AsyncWebServerResponse* wledScreenshotResponse(uint8_t source, uint16_t& outW, uint16_t& outH) {
  outW = 0;
  outH = 0;

  if (!shotLockTake()) {
    USER_PRINTLN("screenshot: another screenshot is running");
    return nullptr;
  }

  uint16_t w = 0;
  uint16_t h = 0;
#if defined(CONFIG_IDF_TARGET_ESP32P4)
  if (source == WLED_SHOT_HDMI) {
    w = displayWidth();
    h = displayHeight();
  }
#endif
  if (!w || !h) {
    w = Segment::maxWidth;
    h = Segment::maxHeight;
  }
  if (!w || !h || (w & 7)) {
    shotLockGive();
    return nullptr;
  }

  // The picture, in the encoder's input buffer - there is no second copy.
  //
  // The JPEG's buffer comes out of the SAME allocation, right after the
  // picture.  Two separate allocations can land on top of each other - and
  // when they do, the picture's write-back lands on the JPEG's header and
  // the JPEG comes out with the picture written over its first bytes.
  const size_t picBytes = (size_t)w * h * 3;
  const size_t jpegCap  = (size_t)w * h + 65536;
  uint8_t* in = allocShotBuffer(picBytes + jpegCap + 128);
  if (!in) {
    USER_PRINTLN("screenshot: no room for the picture");
    shotLockGive();
    return nullptr;
  }
  uint8_t* jpeg = in + ((picBytes + 127) & ~(size_t)127);

  bool ok = false;
#if defined(CONFIG_IDF_TARGET_ESP32P4)
  if (source == WLED_SHOT_HDMI) ok = grabHdmPicture(in, w, h);
  else
#endif
  ok = grabBusPicture(in, w, h);
  if (!ok) {
    free(in);
    shotLockGive();
    return nullptr;
  }

  size_t jpegLen = 0;
  ok = encodeShotPicture(in, w, h, 90, jpeg, jpegCap, jpegLen, outW, outH);
  shotLockGive();

  if (!ok) {
    free(in);
    return nullptr;
  }

  // The JPEG has to outlive this function - the response sends it - so the
  // whole allocation goes with it, and the response frees it.
  return new ShotJpegResponse(in, jpegLen, jpeg - in);
}

#endif // WLED_HAS_SCREENSHOT

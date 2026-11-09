#include "wled_screenshot.h"

#if WLED_HAS_SCREENSHOT

#include "wled.h"
#include <esp_heap_caps.h>
#include <esp_cache.h>
#include "driver/jpeg_encode.h"

#if defined(WLEDMM_DISPLAY_MODE)
// The HDMI mode's runtime size (wled_hdmi.cpp sets them when the panel comes up).
extern uint16_t wledmm_display_w;
extern uint16_t wledmm_display_h;
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
static uint8_t* allocShotBuffer(size_t size) {
  const size_t aligned = (size + 63) & ~(size_t)63;
  uint8_t* p = (uint8_t*)heap_caps_aligned_alloc(64, aligned,
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  if (!p) p = (uint8_t*)heap_caps_aligned_alloc(64, aligned,
                          MALLOC_CAP_DMA | MALLOC_CAP_8BIT);   // internal, if there is room
  return p;
}

// The CPU wrote the buffer, the DMA reads it: the cache goes to memory.
static void shotCacheWriteback(void* addr, size_t size) {
  esp_cache_msync(addr, size, ESP_CACHE_MSYNC_FLAG_DIR_C2M
                                | ESP_CACHE_MSYNC_FLAG_INVALIDATE
                                | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
}

// The DMA wrote the buffer, the CPU reads it: the cache goes to the DMA.
static void shotCacheInvalidate(void* addr, size_t size) {
  esp_cache_msync(addr, size, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
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
static bool encodeShotPicture(uint8_t* in, uint16_t w, uint16_t h, uint8_t quality,
                              uint8_t*& outBuf, size_t& outLen, uint16_t& outW, uint16_t& outH) {
  outBuf = nullptr;
  outLen = 0;
  if (!in || !w || !h) return false;
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
  const size_t outCap = (size_t)w * h + 65536;

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

      size_t outAlloc = 0;
      uint8_t* out = allocShotBuffer(outCap);
      if (!out) {
        USER_PRINTLN("screenshot: no room for the JPEG");
        return false;
      }

      // The CPU filled the picture, the DMA reads it.
      shotCacheWriteback(in, (size_t)w * hh * 3);

      uint32_t outSize = 0;
      const esp_err_t err = jpeg_encoder_process(enc, &cfg, in, (uint32_t)((size_t)w * hh * 3),
                                                  out, (uint32_t)outCap, &outSize);
      if (err == ESP_OK && outSize > 0) {
        // The DMA wrote the JPEG, the CPU reads it.
        shotCacheInvalidate(out, outSize);
        outBuf = out;
        outLen = outSize;
        outW   = w;
        outH   = hh;
        return true;
      }
      free(out);
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

#if defined(WLEDMM_DISPLAY_MODE)
// The HDMI framebuffer: what the monitor shows (the canvas, scaled up).
//
// The framebuffer is already in the encoder's order (the PPA swaps the
// red and the blue when it writes there), so it is only copied.
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
  memcpy(rgb, fb, size);
  return true;
}
#endif

// THE RESPONSE.
//
// It streams the JPEG straight out of the buffer the encoder made - the
// buffer lives in PSRAM, and the web server never copies it into the
// internal heap.  When the response is deleted the buffer goes with it.
class ShotJpegResponse : public AsyncAbstractResponse {
  uint8_t* _buf;
  size_t   _len;
  size_t   _read;

public:
  ShotJpegResponse(uint8_t* buf, size_t len)
    : _buf(buf), _len(len), _read(0) {
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
      memcpy(data, _buf + _read, len);
      _read += len;
      return len;
    }
    memcpy(data, _buf + _read, left);
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
#if defined(WLEDMM_DISPLAY_MODE)
  if (source == WLED_SHOT_HDMI) {
    w = wledmm_display_w;
    h = wledmm_display_h;
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
  uint8_t* in = allocShotBuffer((size_t)w * h * 3);
  if (!in) {
    USER_PRINTLN("screenshot: no room for the picture");
    shotLockGive();
    return nullptr;
  }

  bool ok = false;
#if defined(WLEDMM_DISPLAY_MODE)
  if (source == WLED_SHOT_HDMI) ok = grabHdmPicture(in, w, h);
  else
#endif
  ok = grabBusPicture(in, w, h);
  if (!ok) {
    free(in);
    shotLockGive();
    return nullptr;
  }

  uint8_t* jpeg = nullptr;
  size_t jpegLen = 0;
  ok = encodeShotPicture(in, w, h, 90, jpeg, jpegLen, outW, outH);
  free(in);
  shotLockGive();

  if (!ok) return nullptr;
  return new ShotJpegResponse(jpeg, jpegLen);
}

#endif // WLED_HAS_SCREENSHOT

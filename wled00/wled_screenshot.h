#pragma once
#include <Arduino.h>

// The screenshot API.
//
// The P4 has a hardware JPEG encoder, so the screen can be captured as a
// JPEG and handed to the web server as a picture.  That is what makes the
// small text on the canvas READABLE: the live view is scaled down to a
// handful of pixels and cannot be read, a JPEG of the bus buffer or of the
// HDMI framebuffer can.
//
// The encoder is a P4 (and only a P4) thing, so on any other target the API
// is simply not there.
#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(SOC_JPEG_ENCODE_SUPPORTED)
  #define WLED_HAS_SCREENSHOT 1
#else
  #define WLED_HAS_SCREENSHOT 0
#endif

#if WLED_HAS_SCREENSHOT

class AsyncWebServerResponse;

// WHERE THE PICTURE COMES FROM.
enum WledShotSource : uint8_t {
  WLED_SHOT_BUS  = 0,   // the LED bus buffer - the canvas at its native resolution
  WLED_SHOT_HDMI = 1    // the HDMI framebuffer - what the monitor shows
};

// The JPEG of the screen, as a response the web server can send.
//
// The response OWNS the picture and frees it when the send is done, so the
// caller must NOT free anything: the buffer never has to be copied into the
// web server's own memory, which matters because a 720p picture is far too
// big for the internal heap.
//
// Returns nullptr when the picture could not be made.
AsyncWebServerResponse* wledScreenshotResponse(uint8_t source, uint16_t& outW, uint16_t& outH);

#endif // WLED_HAS_SCREENSHOT

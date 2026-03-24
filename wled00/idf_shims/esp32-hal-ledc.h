// esp32-hal-ledc.h shim for WLED IDF builds.
// LovyanGFX Light_PWM.cpp includes this when ARDUINO is defined.
// Provides the Arduino-ESP32 LEDC wrapper API as thin stubs over IDF driver/ledc.h.
#pragma once
#include <driver/ledc.h>
#include <stdint.h>

// Arduino-ESP32 v3.x combined API: ledcAttach(pin, freq, resolution)
static inline bool ledcAttach(uint8_t /*pin*/, uint32_t /*freq*/, uint8_t /*resolution*/) {
    return false; // IDF path handles this via ledc_channel_config/ledc_timer_config
}

// Arduino-ESP32 v2.x legacy API
static inline double ledcSetup(uint8_t /*channel*/, double /*freq*/, uint8_t /*resolution*/) {
    return 0;
}
static inline void ledcAttachPin(uint8_t /*pin*/, uint8_t /*channel*/) {}
static inline void ledcWrite(uint8_t /*channel*/, uint32_t /*duty*/) {}
static inline void ledcDetachPin(uint8_t /*pin*/) {}
static inline double ledcReadFreq(uint8_t /*channel*/) { return 0; }
static inline uint32_t ledcRead(uint8_t /*channel*/) { return 0; }

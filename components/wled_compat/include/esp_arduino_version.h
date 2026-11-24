// esp_arduino_version.h - the Arduino core's version macros.
//
// The core has this in cores/esp32/esp_arduino_version.h.  LovyanGFX and
// other libraries compare ESP_ARDUINO_VERSION against
// ESP_ARDUINO_VERSION_VAL(...), so the macros have to be here even though
// there is no Arduino core behind them.
//
// The numbers are the core's current ones, so that the libraries take the
// same branches they take with the core.

#pragma once

#define ESP_ARDUINO_VERSION_MAJOR 3
#define ESP_ARDUINO_VERSION_MINOR 3
#define ESP_ARDUINO_VERSION_PATCH 0

#define ESP_ARDUINO_VERSION_VAL(major, minor, patch) ((major << 16) | (minor << 8) | (patch))
#define ESP_ARDUINO_VERSION \
    ESP_ARDUINO_VERSION_VAL(ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH)

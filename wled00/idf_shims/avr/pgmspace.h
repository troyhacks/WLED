// avr/pgmspace.h shim — FastLED and some libraries include this on non-AVR.
// On ESP32 / IDF it's a no-op (PROGMEM is already handled by idf_compat.h).
#pragma once

// esp32-hal.h - the Arduino core's HAL header.
//
// FastLED, and WLED's own pin manager, include it.  What it used to carry -
// the GPIO helpers, the pin functions and the timers - is in the Arduino API
// layer now.
//
// This header is included from inside an extern "C" block (wled00/pin_manager.h
// does that), so it must stay C: no C++ headers, no Arduino.h.
//
// The P4's ADC channels are listed here as well, because the Arduino core got
// them from a table that ESP-IDF does not have:
//   ADC1: GPIO 16..23, channels 0..7
//   ADC2: GPIO 49..54, channels 0..5
// The Arduino core numbers the channels across the two units, so ADC2's
// channels are 8..13.

#pragma once

#include "esp_attr.h"
#include "driver/gpio.h"
#include "soc/soc_caps.h"
#include "soc/soc.h"

// The Arduino core's marker for a function that has to live in IRAM.
#ifndef ARDUINO_ISR_ATTR
#define ARDUINO_ISR_ATTR IRAM_ATTR
#endif

// The Arduino core does not name the P4's analog pins A0..A7 - the analog
// pins are ordinary GPIOs there.  The names are in pins_arduino.h, as static
// constants rather than macros, the way the core has them.

#ifndef digitalPinIsValid
#define digitalPinIsValid(pin) GPIO_IS_VALID_GPIO(pin)
#endif
#ifndef digitalPinCanOutput
#define digitalPinCanOutput(pin) GPIO_IS_VALID_OUTPUT_GPIO(pin)
#endif

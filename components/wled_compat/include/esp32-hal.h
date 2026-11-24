// esp32-hal.h - the Arduino core's HAL header.
//
// FastLED, and WLED's own pin manager, include it.  What it used to carry -
// the GPIO helpers, the pin functions and the timers - is in the Arduino API
// layer now, so this brings that in.
//
// The P4's ADC channels are listed here as well, because the Arduino core got
// them from a table that ESP-IDF does not have:
//   ADC1: GPIO 16..23, channels 0..7
//   ADC2: GPIO 49..54, channels 0..5
// The Arduino core numbers the channels across the two units, so ADC2's
// channels are 8..13.

#pragma once

#include "Arduino.h"
#include "driver/gpio.h"

#ifndef SOC_ADC_PERIPH_NUM
#define SOC_ADC_PERIPH_NUM 2
#endif
#ifndef SOC_ADC_MAX_CHANNEL_NUM
#define SOC_ADC_MAX_CHANNEL_NUM 8
#endif

// The Arduino core's own names for the analog pins.  The P4 has no A0..A7
// pins as such - the analog pins are ordinary GPIOs - but the names have to
// exist for the code that uses them.
#define A0 16
#define A1 17
#define A2 18
#define A3 19
#define A4 20
#define A5 21
#define A6 22
#define A7 23
#define A8 49
#define A9 50
#define A10 51
#define A11 52
#define A12 53
#define A13 54

#ifndef digitalPinIsValid
#define digitalPinIsValid(pin) GPIO_IS_VALID_GPIO(pin)
#endif
#ifndef digitalPinCanOutput
#define digitalPinCanOutput(pin) GPIO_IS_VALID_OUTPUT_GPIO(pin)
#endif

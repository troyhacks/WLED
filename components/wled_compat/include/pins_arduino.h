// pins_arduino.h - the Arduino core's pin names for the P4.
//
// The core has a pins_arduino.h per board variant, and the P4's is in
// variants/esp32p4/.  These are the same names, the same values and the same
// spelling - static constants, not macros, because a macro named A8 would
// break any code that uses A8 as an identifier.

#pragma once

#include <stdint.h>
#include "soc/soc_caps.h"

// BOOT_MODE 35
// BOOT_MODE2 36 pullup

static const uint8_t TX = 37;
static const uint8_t RX = 38;

static const uint8_t SDA = 7;
static const uint8_t SCL = 8;

// Use GPIOs 36 or lower on the P4 DevKit to avoid LDO power issues with high
// numbered GPIOs.
static const uint8_t SS = 26;
static const uint8_t MOSI = 32;
static const uint8_t MISO = 33;
static const uint8_t SCK = 36;

static const uint8_t A0 = 16;
static const uint8_t A1 = 17;
static const uint8_t A2 = 18;
static const uint8_t A3 = 19;
static const uint8_t A4 = 20;
static const uint8_t A5 = 21;
static const uint8_t A6 = 22;
static const uint8_t A7 = 23;
static const uint8_t A8 = 49;
static const uint8_t A9 = 50;
static const uint8_t A10 = 51;
static const uint8_t A11 = 52;
static const uint8_t A12 = 53;
static const uint8_t A13 = 54;

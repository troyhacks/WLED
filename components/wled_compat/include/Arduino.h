// Arduino.h - WLED-MM-P4's Arduino API layer for the pure ESP-IDF build.
//
// There is no Arduino core in this firmware.  What the Arduino core used to
// provide - the String class, Print/Stream, the GPIO and LEDC helpers, the
// millis() clock - is here, backed by ESP-IDF drivers instead.
//
// The header names and the semantics match the ESP32 Arduino core exactly, so
// the WLED sources and the Arduino libraries (AsyncTCP, ESPAsyncWebServer,
// NeoPixelBus, LovyanGFX) compile against this without a single change.
//
// This file is part of WLED-MM-P4.  See LICENSE-NOTICE.md for the provenance of
// the vendored files next to it.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "esp_attr.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// The environment macros the Arduino libraries branch on.  The Arduino core is
// gone, but AsyncTCP only pulls in IPAddress.h under `#ifdef ARDUINO` and
// ESPAsyncWebServer only recognises the ESP32 under `#ifdef ESP32`, so the
// macros still have to be there.
#ifndef ARDUINO
#define ARDUINO 10819
#endif
#ifndef ARDUINO_ARCH_ESP32
#define ARDUINO_ARCH_ESP32
#endif
#ifndef ESP32
#define ESP32 1
#endif
#ifndef ARDUINO_USB_MODE
#define ARDUINO_USB_MODE 1
#endif
#ifndef ARDUINO_USB_CDC_ON_BOOT
#define ARDUINO_USB_CDC_ON_BOOT 0
#endif

// PROGMEM, PSTR, pgm_read_* - on the ESP32 (the P4 included) the flash is
// memory mapped, so these are the identity.  This is what the Arduino core
// does too (cores/esp32/pgmspace.h).
#include <pgmspace.h>

// The String class, Print and Stream.
#include "WString.h"
#include "Print.h"
#include "Stream.h"
#include "Printable.h"
#include "WCharacter.h"
#include "IPAddress.h"

typedef uint8_t byte;
typedef bool boolean;
typedef unsigned int word;

#define HIGH 0x1
#define LOW  0x0

#define INPUT        0x0
#define OUTPUT       0x1
#define INPUT_PULLUP 0x2
#define INPUT_PULLDOWN 0x3
#define OUTPUT_OPEN_DRAIN 0x4
#define INPUT_OUTPUT 0x5

#define LSBFIRST 0
#define MSBFIRST 1

#define CHANGE 1
#define FALLING 2
#define RISING 3

#ifndef LED_BUILTIN
#define LED_BUILTIN 0
#endif

// --- GPIO ------------------------------------------------------------------
void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t val);
int digitalRead(uint8_t pin);
int analogRead(uint8_t pin);
void analogReadResolution(uint8_t bits);
void analogWrite(uint8_t pin, int value);
void analogWriteFrequency(uint8_t pin, uint32_t freq);
void analogWriteResolution(uint8_t pin, uint8_t bits);
void analogReadResolution(uint8_t pin, uint8_t bits);

// --- the clock -------------------------------------------------------------
unsigned long millis();
unsigned long micros();
void delay(unsigned long ms);
void delayMicroseconds(unsigned int us);
void yield();

// --- random ----------------------------------------------------------------
long random(long howbig);
long random(long howsmall, long howbig);
void randomSeed(unsigned long seed);

// --- maths -----------------------------------------------------------------
#ifndef ARDUINO_MIN
#define ARDUINO_MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef ARDUINO_MAX
#define ARDUINO_MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef min
#define min(a, b) ARDUINO_MIN(a, b)
#endif
#ifndef max
#define max(a, b) ARDUINO_MAX(a, b)
#endif
#ifndef constrain
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#endif
#ifndef sq
#define sq(x) ((x) * (x))
#endif
#ifndef abs
#define abs(x) ((x) > 0 ? (x) : -(x))
#endif
#ifndef round
#define round(x) ((x) >= 0 ? (long)((x) + 0.5) : (long)((x) - 0.5))
#endif
#ifndef floor
#define floor(x) ((long)(x))
#endif
#ifndef ceil
#define ceil(x) (((x) > 0) ? ((long)(x) + 1) : (long)(x))
#endif
long map(long x, long in_min, long in_max, long out_min, long out_max);
#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define bitWrite(value, bit, setvalue) ((value) = ((setvalue) ? ((value) | (1UL << (bit))) : ((value) & ~(1UL << (bit)))))
#define bitSet(value, bit) ((value) |= (1UL << (bit)))
#define bitClear(value, bit) ((value) &= ~(1UL << (bit)))
#define lowByte(w) ((uint8_t)((w) & 0xff))
#define highByte(w) ((uint8_t)((w) >> 8))
#define lowWord(w) ((uint16_t)((w) & 0xffff))
#define highWord(w) ((uint16_t)((w) >> 16))

// --- interrupts ------------------------------------------------------------
typedef void (*arduino_isr_handler_t)(void *);
void attachInterrupt(uint8_t pin, arduino_isr_handler_t handler, int mode);
void detachInterrupt(uint8_t pin);
void interrupts();
void noInterrupts();

// --- the chip --------------------------------------------------------------
// The Arduino core's "ESP" object.  WLED asks it for the heap, the PSRAM, the
// chip revision and the flash size.
class EspClass {
public:
    uint32_t getChipId();
    const char *getChipModel();
    uint8_t getChipRevision();
    uint8_t getChipCores();
    uint32_t getCpuFreqMHz();
    uint32_t getFlashChipSize();
    uint32_t getFlashChipRealSize();
    uint8_t getFlashChipMode();
    uint32_t getFlashChipSpeed();
    uint32_t getHeapSize();
    uint32_t getFreeHeap();
    uint32_t getMinFreeHeap();
    uint32_t getMaxAllocHeap();
    uint32_t getPsramSize();
    uint32_t getFreePsram();
    uint32_t getMinFreePsram();
    uint32_t getMaxAllocPsram();
    uint32_t getMaxFreeBlockSize();
    const char *getSdkVersion();
    const char *getCoreVersion();
    void restart();
    uint8_t getResetReason();
    void *getResetInfoPtr();
    uint32_t getFreeSketchSpace();
};
extern EspClass ESP;

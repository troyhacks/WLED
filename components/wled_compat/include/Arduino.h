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
#include <algorithm>
#include <cmath>
#include <list>
#include <vector>

#include "esp_attr.h"
#include "esp_system.h"
#include "esp_idf_version.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "soc/soc_caps.h"
#include "soc/soc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// The Arduino core's HAL header, for ARDUINO_ISR_ATTR.  It is C, because
// WLED includes it from inside an extern "C" block.
#include "esp32-hal.h"

// The board's pin names: TX, RX, SDA, SCL, SS, MOSI, MISO, SCK and A0..A13.
#include "pins_arduino.h"

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

// The basic types come first: the Arduino classes below use them.
typedef uint8_t byte;
typedef bool boolean;
typedef unsigned int word;

// The Arduino core's word(): makeWord() with one or two bytes.  The macro
// wins over the typedef above, exactly as in the core.
uint16_t makeWord(uint16_t w);
uint16_t makeWord(uint8_t h, uint8_t l);
#define word(...) makeWord(__VA_ARGS__)

// The String class, Print and Stream.
#include "WString.h"
#include "Print.h"
#include "Stream.h"
#include "Printable.h"
#include "WCharacter.h"
#include "IPAddress.h"

// The maths helpers come from the standard library, the way the Arduino core
// does it: a macro named min or max collides with <algorithm>.
using std::abs;
using std::isinf;
using std::isnan;
using std::max;
using std::min;
using std::round;

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

// The clock, the way the Arduino core has it.
#ifndef F_CPU
#define F_CPU (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000U)
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

// The Arduino core's LEDC calls.  ledcWrite() takes a pin, like the core's.
bool ledcAttachChannel(uint8_t pin, uint32_t freq, uint8_t resolution, uint8_t channel);
bool ledcWrite(uint8_t pin, uint32_t duty);
bool ledcWriteChannel(uint8_t channel, uint32_t duty);

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
// The Arduino core cannot define max() and min() as macros - they collide
// with the C++ standard library - so it brings the standard ones in instead.
// Same here.
#define _min(a, b) ((a) < (b) ? (a) : (b))
#define _max(a, b) ((a) > (b) ? (a) : (b))
#ifndef constrain
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#endif
#ifndef sq
#define sq(x) ((x) * (x))
#endif
#ifndef DEG_TO_RAD
#define DEG_TO_RAD 0.017453292519943295769236907684886
#endif
#ifndef RAD_TO_DEG
#define RAD_TO_DEG 57.295779513082320876798154814105
#endif
#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif
#ifndef HALF_PI
#define HALF_PI 1.5707963267948966192313216916398
#endif
#ifndef TWO_PI
#define TWO_PI 6.283185307179586476925286766559
#endif
#ifndef radians
#define radians(deg) ((deg) * DEG_TO_RAD)
#endif
#ifndef degrees
#define degrees(rad) ((rad) * RAD_TO_DEG)
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

// --- the pin mapping helpers -----------------------------------------------
// The Arduino core has these in esp32-hal-gpio.c; WLED's pin manager and the
// Arduino libraries ask for them.
int8_t digitalPinToAnalogChannel(uint8_t pin);
int8_t analogChannelToDigitalPin(uint8_t channel);
int8_t digitalPinToTouchChannel(uint8_t pin);
uint8_t digitalPinToPort(uint8_t pin);
uint32_t digitalPinToBitMask(uint8_t pin);
uint8_t digitalPinToInterrupt(uint8_t pin);
const char *digitalPinToPinName(uint8_t pin);
bool digitalPinHasPWM(uint8_t pin);

// UART0's pins are fixed by the bootloader.  The Arduino core has the table
// for every SoC; here is the P4's.
#ifndef SOC_RX0
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#define SOC_RX0 (gpio_num_t)38
#endif
#endif
#ifndef SOC_TX0
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#define SOC_TX0 (gpio_num_t)37
#endif
#endif

// The names of UART0's pins, the way the Arduino core has them.
#ifndef RX
#define RX SOC_RX0
#endif
#ifndef TX
#define TX SOC_TX0
#endif

// The Arduino core's task creator, which picks the core.
BaseType_t xTaskCreateUniversal(TaskFunction_t function, const char *name, uint32_t stackDepth,
                                 void *parameters, UBaseType_t priority, TaskHandle_t *handle,
                                 BaseType_t core);

// The Arduino core's PSRAM allocators.
void *ps_malloc(size_t size);
void *ps_calloc(size_t size, size_t count);
void *ps_realloc(void *ptr, size_t size);
void ps_free(void *ptr);

// Is there PSRAM on this board?
bool psramFound();

// Start listening for the network events and hand them to the application's
// WiFiEvent(), the way the Arduino core did.
void wledNetworkEventsBegin();

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

// The serial classes, and the "Serial" name.  This comes last, because the
// serial classes are built on the String and the Print above.
#include "HardwareSerial.h"

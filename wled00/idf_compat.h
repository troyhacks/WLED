// idf_compat.h — Arduino API shims for pure ESP-IDF builds.
// Included instead of Arduino.h when WLED_IDF_BUILD is defined.
// Provides: millis/micros/delay, GPIO, random, Serial stub, math macros.
// Does NOT provide: String class (handled by String→std::string migration).
#pragma once

// Claim ARDUINO_ARCH_ESP32 and ESP32 so that existing
// "#ifdef ARDUINO_ARCH_ESP32 / #else(8266)" guards select the ESP32 branch.
#ifndef ARDUINO_ARCH_ESP32
#define ARDUINO_ARCH_ESP32 1
#endif
#ifndef ESP32
#define ESP32 1
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <algorithm>
#include <string>

// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

// IDF core
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"

// GPIO
#include "driver/gpio.h"

// ─── Primitive types ──────────────────────────────────────────────────────────
typedef uint8_t  byte;
// Note: 'word' is NOT typedefed to uint16_t here — doing so conflicts with the
// Arduino word(high, low) two-argument helper function used in Toki.h etc.
// Use uint16_t directly when a 16-bit type is needed.
typedef bool     boolean;

// word(high, low) — combine two bytes into a 16-bit value (Arduino compat)
static inline uint16_t word(uint8_t h, uint8_t l) { return (uint16_t)((h << 8) | l); }
static inline uint16_t word(uint16_t w)            { return w; }

// ─── Timing ───────────────────────────────────────────────────────────────────
static inline uint32_t millis()  { return (uint32_t)(esp_timer_get_time() / 1000ULL); }
static inline uint32_t micros()  { return (uint32_t)esp_timer_get_time(); }
static inline void     delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static inline void     delayMicroseconds(uint32_t us) {
    if (us >= 1000) vTaskDelay(pdMS_TO_TICKS(us / 1000));
    else            esp_rom_delay_us(us);
}
static inline void yield() { vPortYield(); }

// xTaskCreateUniversal: Arduino-ESP32 helper — pins task to core if valid, else unpinned.
static inline BaseType_t xTaskCreateUniversal(
    TaskFunction_t pvTaskCode, const char* const pcName,
    const uint32_t usStackDepth, void* const pvParameters,
    UBaseType_t uxPriority, TaskHandle_t* const pvCreatedTask,
    const BaseType_t xCoreID) {
    if (xCoreID >= 0 && xCoreID < portNUM_PROCESSORS)
        return xTaskCreatePinnedToCore(pvTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pvCreatedTask, xCoreID);
    return xTaskCreate(pvTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pvCreatedTask);
}

// ─── Math ─────────────────────────────────────────────────────────────────────
using std::min;
using std::max;
using std::abs;

#ifndef constrain
#define constrain(val, lo, hi) ((val) < (lo) ? (lo) : ((val) > (hi) ? (hi) : (val)))
#endif
// Use an inline function instead of a macro to avoid conflicting with std::map<K,V>
#ifndef map
inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}
#endif
#ifndef sq
#define sq(x) ((x)*(x))
#endif
#ifndef PI
#define PI 3.14159265358979323846f
#endif
#ifndef TWO_PI
#define TWO_PI (2.0f * PI)
#endif
#ifndef HALF_PI
#define HALF_PI (0.5f * PI)
#endif
#ifndef DEG_TO_RAD
#define DEG_TO_RAD (PI / 180.0f)
#endif
#ifndef RAD_TO_DEG
#define RAD_TO_DEG (180.0f / PI)
#endif

// ─── Random ───────────────────────────────────────────────────────────────────
// POSIX random() is long random(void) — these overloads (1 and 2 args) are distinct.
static inline long random(long max_val) {
    if (max_val <= 0) return 0;
    return (long)(esp_random() % (uint32_t)max_val);
}
static inline long random(long min_val, long max_val) {
    if (max_val <= min_val) return min_val;
    return min_val + (long)(esp_random() % (uint32_t)(max_val - min_val));
}
static inline void randomSeed(unsigned long) {}  // IDF uses hardware RNG; seed is ignored

// ─── GPIO ─────────────────────────────────────────────────────────────────────
// Use distinct values that don't collide with gpio_mode_t enum values.
#ifndef INPUT
#define INPUT           0x00
#endif
#ifndef OUTPUT
#define OUTPUT          0x01
#endif
#ifndef INPUT_PULLUP
#define INPUT_PULLUP    0x02
#endif
#ifndef INPUT_PULLDOWN
#define INPUT_PULLDOWN  0x03
#endif
#ifndef OUTPUT_OPEN_DRAIN
#define OUTPUT_OPEN_DRAIN 0x04
#endif
#ifndef HIGH
#define HIGH 1
#endif
#ifndef LOW
#define LOW  0
#endif

static inline void pinMode(uint8_t pin, uint8_t mode) {
    if ((int)pin >= GPIO_NUM_MAX) return;
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << pin);
    cfg.intr_type    = GPIO_INTR_DISABLE;
    cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    switch (mode) {
        case INPUT:           cfg.mode = GPIO_MODE_INPUT;           break;
        case OUTPUT:          cfg.mode = GPIO_MODE_OUTPUT;          break;
        case OUTPUT_OPEN_DRAIN: cfg.mode = GPIO_MODE_OUTPUT_OD;    break;
        case INPUT_PULLUP:
            cfg.mode       = GPIO_MODE_INPUT;
            cfg.pull_up_en = GPIO_PULLUP_ENABLE;
            break;
        case INPUT_PULLDOWN:
            cfg.mode           = GPIO_MODE_INPUT;
            cfg.pull_down_en   = GPIO_PULLDOWN_ENABLE;
            break;
        default:              cfg.mode = GPIO_MODE_DISABLE;         break;
    }
    gpio_config(&cfg);
}
static inline void    digitalWrite(uint8_t pin, uint8_t val) { gpio_set_level((gpio_num_t)pin, val); }
static inline int     digitalRead(uint8_t pin)               { return gpio_get_level((gpio_num_t)pin); }
static inline int     analogRead(uint8_t)                    { return 0; }  // stub — use IDF ADC API directly
static inline void    analogWrite(uint8_t, int)              {}             // stub — use IDF LEDC API directly

// ─── PROGMEM / Flash string helpers (no-op on ESP32) ──────────────────────────
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef PSTR
#define PSTR(s) (s)
#endif
#ifndef F
#define F(s) (s)
#endif
// Use macros (not inline functions) so that LovyanGFX's pgmspace.h
// (which checks `#if defined(ARDUINO) && !defined(pgm_read_byte)`) sees
// pgm_read_byte as already defined and skips its own conflicting #define.
#ifndef pgm_read_byte
#define pgm_read_byte(p)  (*(const uint8_t*)(p))
#endif
#ifndef pgm_read_word
#define pgm_read_word(p)  (*(const uint16_t*)(p))
#endif
#ifndef pgm_read_dword
#define pgm_read_dword(p) (*(const uint32_t*)(p))
#endif
#ifndef pgm_read_byte_near
#define pgm_read_byte_near(p)  pgm_read_byte(p)
#endif
#ifndef pgm_read_word_near
#define pgm_read_word_near(p)  pgm_read_word(p)
#endif
// P-string functions (PROGMEM no-ops on ESP32)
#ifndef strcpy_P
#define strcpy_P(dst, src)        strcpy(dst, src)
#endif
#ifndef strcat_P
#define strcat_P(dst, src)        strcat(dst, src)
#endif
#ifndef strlen_P
#define strlen_P(src)             strlen(src)
#endif
#ifndef strcmp_P
#define strcmp_P(a, b)            strcmp(a, b)
#endif
#ifndef sprintf_P
#define sprintf_P(buf, fmt, ...)  sprintf(buf, fmt, ##__VA_ARGS__)
#endif
#ifndef snprintf_P
#define snprintf_P(buf, n, fmt, ...) snprintf(buf, n, fmt, ##__VA_ARGS__)
#endif

// ─── PSRAM helpers ────────────────────────────────────────────────────────────
#ifdef CONFIG_SPIRAM
#include "esp_psram.h"
#ifndef psramFound
#define psramFound() (esp_psram_is_initialized())
#endif
#else
#ifndef psramFound
#define psramFound() (false)
#endif
#endif // CONFIG_SPIRAM

// ─── GPIO count (used by xml.cpp for d.max_gpio) ──────────────────────────────
#ifndef NUM_DIGITAL_PINS
#define NUM_DIGITAL_PINS ((int)GPIO_NUM_MAX)
#endif

// ─── dtostrf (AVR libc function — not in newlib; format a float to a string) ──
#ifndef dtostrf
static inline char* dtostrf(double val, int width, int prec, char* s) {
    snprintf(s, 32, "%*.*f", width, prec, val);
    return s;
}
#endif

// ─── ADC pin helpers ──────────────────────────────────────────────────────────
#ifndef digitalPinToAnalogChannel
// Returns -1 for pins that don't support ADC (conservative: 0 means valid channel 0)
#define digitalPinToAnalogChannel(p) ((p) >= 0 ? 0 : -1)
#endif

// ─── LEDC stubs (Arduino LEDC API → IDF LEDC shim) ───────────────────────────
#include "driver/ledc.h"
#ifndef ledcSetup
static inline uint32_t ledcSetup(uint8_t chan, double freq, uint8_t bits) {
    ledc_timer_config_t t = {};
    t.speed_mode      = LEDC_LOW_SPEED_MODE;
    t.timer_num       = (ledc_timer_t)(chan / 8);
    t.duty_resolution = (ledc_timer_bit_t)bits;
    t.freq_hz         = (uint32_t)freq;
    t.clk_cfg         = LEDC_AUTO_CLK;
    ledc_timer_config(&t);
    return (uint32_t)freq;
}
#endif
#ifndef ledcAttachPin
static inline void ledcAttachPin(uint8_t pin, uint8_t chan) {
    ledc_channel_config_t c = {};
    c.gpio_num   = pin;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel    = (ledc_channel_t)(chan % 8);
    c.timer_sel  = (ledc_timer_t)(chan / 8);
    c.duty       = 0;
    c.hpoint     = 0;
    ledc_channel_config(&c);
}
#endif
#ifndef ledcDetachPin
static inline void ledcDetachPin(uint8_t pin) { gpio_reset_pin((gpio_num_t)pin); }
#endif
#ifndef ledcWrite
static inline void ledcWrite(uint8_t chan, uint32_t duty) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)(chan % 8), duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)(chan % 8));
}
#endif

// ─── Compiler attribute macros (may already be defined by IDF) ────────────────
#ifndef IRAM_ATTR
#define IRAM_ATTR  __attribute__((section(".iram.text")))
#endif
#ifndef DRAM_ATTR
#define DRAM_ATTR  __attribute__((section(".dram.data")))
#endif

// ─── Serial stub — routes to printf() on UART0 (default IDF console) ─────────
// Full Serial migration (Serial.* → printf()) is handled per-file.
// This stub lets code that still uses Serial compile during incremental migration.
struct _WledSerial {
    void begin(unsigned long, ...) {}
    int  available() { return 0; }
    int  read()      { return -1; }
    int  read(uint8_t* buf, size_t len) { (void)buf; (void)len; return 0; }
    void flush()     {}
    void setTimeout(unsigned long) {}   // no-op — IDF UART has no stream timeout concept
    void setTxTimeoutMs(uint32_t)  {}   // Arduino CDC-on-boot stub
    void setDebugOutput(bool)      {}   // Arduino debug output stub

    size_t print(const char* s)        { return printf("%s", s ? s : ""); }
    size_t print(char c)               { return printf("%c", c); }
    size_t print(int v, int = 10)      { return printf("%d", v); }
    size_t print(unsigned v, int = 10) { return printf("%u", v); }
    size_t print(long v, int = 10)     { return printf("%ld", v); }
    size_t print(unsigned long v, int = 10) { return printf("%lu", v); }
    size_t print(float v, int d = 2)   { return printf("%.*f", d, (double)v); }
    size_t print(double v, int d = 2)  { return printf("%.*f", d, v); }

    // String overloads (String extends std::string; forward to c_str())
    // Must be declared after the String class is defined (WString_compat.h is
    // included later in this file), so these are defined inline here using the
    // forward declaration — the compiler resolves them at call sites which are
    // always after WString_compat.h has been seen.
    // We use a template-based forwarding trick to defer the String type dependency.
    template<typename S, typename = typename S::value_type>
    size_t print(const S& s)   { return print(s.c_str()); }
    template<typename S, typename = typename S::value_type>
    size_t println(const S& s) { return println(s.c_str()); }

    size_t println(const char* s)      { return printf("%s\n", s ? s : ""); }
    size_t println(char c)             { return printf("%c\n", c); }
    size_t println(int v, int = 10)    { return printf("%d\n", v); }
    size_t println(unsigned v, int = 10) { return printf("%u\n", v); }
    size_t println(long v, int = 10)   { return printf("%ld\n", v); }
    size_t println(unsigned long v, int = 10) { return printf("%lu\n", v); }
    size_t println(float v, int d = 2) { return printf("%.*f\n", d, (double)v); }
    size_t println(double v, int d = 2){ return printf("%.*f\n", d, v); }
    size_t println()                   { return printf("\n"); }

    template<typename... Args>
    size_t printf(const char* fmt, Args... args) { return ::printf(fmt, args...); }

    // printf_P: Arduino AVR progmem variant — on ESP32 PSTR is a no-op so identical
    template<typename... Args>
    size_t printf_P(const char* fmt, Args... args) { return ::printf(fmt, args...); }

    int  peek()  { return -1; }   // no buffered read-ahead in this stub
    void end()   {}               // Arduino Serial.end() — stop UART; no-op here

    // operator bool: allows "if (Serial)" checks
    explicit operator bool() const { return true; }

    // Binary write methods (needed by Improv WiFi and other serial protocols)
    size_t write(uint8_t b) { return ::putchar((int)b) == EOF ? 0 : 1; }
    size_t write(const uint8_t* buf, size_t len) {
        return fwrite(buf, 1, len, stdout);
    }
    size_t write(const char* buf, size_t len) {
        return fwrite(buf, 1, len, stdout);
    }
};

extern _WledSerial Serial;
// Serial1/Serial2 stubs if needed
extern _WledSerial Serial1;
extern _WledSerial Serial2;

// ─── String compatibility class ───────────────────────────────────────────────
#include "WString_compat.h"

// ─── ArduinoJson v6 integration flags ────────────────────────────────────────
// Disable Arduino-specific ArduinoJson overloads that require Print / Stream /
// Printable / __FlashStringHelper — those Arduino types don't exist here.
// String integration (concat) IS enabled; String class is defined above.
// These must be defined before ArduinoJson-v6.h is first included.
#ifndef ARDUINOJSON_ENABLE_ARDUINO_PRINT
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT  0
#endif
#ifndef ARDUINOJSON_ENABLE_ARDUINO_STREAM
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#endif
#ifndef ARDUINOJSON_ENABLE_PROGMEM
#define ARDUINOJSON_ENABLE_PROGMEM        0
#endif
// Provide __FlashStringHelper as a forward declaration so any code that
// references F()-wrapped strings as const __FlashStringHelper* compiles.
class __FlashStringHelper;

// ─── IPAddress class (Arduino compat) ─────────────────────────────────────────
// Included here so all files that pull in idf_compat.h get IPAddress.
#include "idf_shims/IPAddress.h"
#include "idf_shims/Wire.h"
#include "idf_shims/SPI.h"

// ─── Bit manipulation macros (Arduino compat) ─────────────────────────────────
#ifndef bitRead
#define bitRead(x, n)      (((x) >> (n)) & 1)
#endif
#ifndef bitSet
#define bitSet(x, n)       ((x) |= (1UL << (n)))
#endif
#ifndef bitClear
#define bitClear(x, n)     ((x) &= ~(1UL << (n)))
#endif
#ifndef bitWrite
#define bitWrite(x, n, v)  ((v) ? bitSet(x, n) : bitClear(x, n))
#endif
#ifndef bit
#define bit(n)             (1UL << (n))
#endif

// ─── PSRAM allocation helpers ─────────────────────────────────────────────────
#include "esp_heap_caps.h"
#ifndef ps_malloc
#define ps_malloc(size)         heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#endif
#ifndef ps_calloc
#define ps_calloc(n, size)      heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#endif
#ifndef ps_realloc
#define ps_realloc(ptr, size)   heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#endif

// ─── Math helpers ─────────────────────────────────────────────────────────────
#ifndef radians
#define radians(deg) ((deg) * (M_PI / 180.0))
#endif
#ifndef degrees
#define degrees(rad) ((rad) * (180.0 / M_PI))
#endif
#ifndef sq
#define sq(x) ((x) * (x))
#endif

// ─── Additional PGMSPACE aliases ──────────────────────────────────────────────
#ifndef memcmp_P
#define memcmp_P(a, b, n)       memcmp(a, b, n)
#endif
#ifndef memcpy_P
#define memcpy_P(dst, src, n)   memcpy(dst, src, n)
#endif
#ifndef memmove_P
#define memmove_P(dst, src, n)  memmove(dst, src, n)
#endif
#ifndef strncmp_P
#define strncmp_P(a, b, n)      strncmp(a, b, n)
#endif
#ifndef strlcpy_P
#define strlcpy_P(dst, src, n)  (strncpy(dst, src, (n)-1), (dst)[(n)-1]='\0', strlen(src))
#endif
#ifndef strncpy_P
#define strncpy_P(dst, src, n)  strncpy(dst, src, n)
#endif

// ─── lwIP version (Arduino uses LWIP_VERSION_MAJOR for feature detection) ─────
#include <lwip/lwip_napt.h>   // pulls lwip_version.h transitively on IDF v5
#ifndef LWIP_VERSION_MAJOR
#include <lwip/opt.h>
#endif

// ─── ESP object shim (Arduino ESP32 EspClass) ─────────────────────────────────
// json.cpp and other files call ESP.getFreeHeap() etc.
#include "esp_system.h"
#include "esp_psram.h"
#include "esp_flash.h"
#include "esp_chip_info.h"
struct _EspClass {
    uint32_t getFreeHeap()      { return esp_get_free_heap_size(); }
    uint32_t getHeapSize()      { return heap_caps_get_total_size(MALLOC_CAP_INTERNAL); }
    uint32_t getMinFreeHeap()   { return esp_get_minimum_free_heap_size(); }
    uint32_t getMaxAllocHeap()  { return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL); }
    uint32_t getFlashChipSize() {
        uint32_t size = 0;
        esp_flash_get_size(NULL, &size);
        return size;
    }
    uint32_t getPsramSize()     {
#ifdef CONFIG_SPIRAM
        return esp_psram_get_size();
#else
        return 0;
#endif
    }
    uint32_t getFreePsram()     { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
    uint32_t getMinFreePsram()  { return heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM); }
    uint32_t getCpuFreqMHz()    { return CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ; }
    uint32_t getSketchSize()    { return 0; }
    uint32_t getFreeSketchSpace(){ return 0; }
    uint32_t getFlashChipSpeed(){ return 80000000; }  // 80 MHz typical QSPI; no IDF API for this
    void restart()              { esp_restart(); }
    const char* getSdkVersion() { return esp_get_idf_version(); }
    uint8_t  getChipRevision()  {
        esp_chip_info_t ci; esp_chip_info(&ci);
        return (uint8_t)ci.revision;
    }
    uint8_t  getChipCores()     {
        esp_chip_info_t ci; esp_chip_info(&ci);
        return (uint8_t)ci.cores;
    }
    const char* getChipModel()  {
#if defined(CONFIG_IDF_TARGET_ESP32P4)
        return "ESP32-P4";
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
        return "ESP32-S3";
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
        return "ESP32-C3";
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
        return "ESP32-C6";
#else
        return "ESP32";
#endif
    }
};
extern _EspClass ESP;

// ─── GPIO validity macros (Arduino ESP32 compat) ──────────────────────────────
// Implement inline to avoid depending on gpio_is_valid_gpio_num() which is not
// exported in all IDF v5 targets/versions. GPIO_NUM_MAX is always available.
static inline bool _wled_gpio_valid(int pin) {
    return pin >= 0 && pin < (int)GPIO_NUM_MAX;
}
#ifndef digitalPinIsValid
#define digitalPinIsValid(pin)   _wled_gpio_valid((int)(pin))
#endif
#ifndef digitalPinCanOutput
#define digitalPinCanOutput(pin) _wled_gpio_valid((int)(pin))
#endif

// ─── Touch / ADC pin channel stubs ────────────────────────────────────────────
// ESP32-P4 has no capacitive touch pins; ESP32 original used these for buttons.
#ifndef digitalPinToTouchChannel
#define digitalPinToTouchChannel(p) (-1)
#endif
// analogInputToDigitalPin: reverse-map of ADC channel → GPIO (not needed for P4)
#ifndef analogInputToDigitalPin
#define analogInputToDigitalPin(ch) (0)
#endif

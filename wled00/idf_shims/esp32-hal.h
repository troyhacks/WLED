// esp32-hal.h shim for WLED IDF builds.
// FastLED and pin_manager.h include this; we provide the Arduino HAL symbols
// they need without pulling in the full Arduino framework.
#pragma once
#include "idf_compat.h"

// ─── GPIO register addresses ─────────────────────────────────────────────────
// FastLED's fastpin_esp32.h uses these directly.
// They are defined in soc/gpio_reg.h which driver/gpio.h doesn't always pull
// in transitively, so include it explicitly.
#include "soc/gpio_reg.h"

// ─── CPU cycle counter ───────────────────────────────────────────────────────
// FastLED's clockless_rmt_esp32.h calls cpu_hal_get_cycle_count().
// In IDF v5 this is defined in esp_hw_support/include/hal/cpu_hal.h.
#include "hal/cpu_hal.h"

// ─── F_CPU ────────────────────────────────────────────────────────────────────
// FastLED's fastled_delay.h and fastspi.h use F_CPU (CPU freq in Hz).
// Prefer the sdkconfig value if available.
#ifndef F_CPU
  #ifdef CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
    #define F_CPU (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000UL)
  #else
    #define F_CPU 400000000UL  // ESP32-P4 default 400 MHz
  #endif
#endif

// ─── Arduino-style GPIO helpers ──────────────────────────────────────────────
// pin_manager.h uses digitalPinHasPWM / digitalPinToInterrupt.
// FastLED's fastpin.h uses digitalPinToBitMask / digitalPinToPort /
// portOutputRegister / portInputRegister.

// All ESP32-P4 GPIO pins support LEDC PWM output.
#ifndef digitalPinHasPWM
#define digitalPinHasPWM(p)     (1)
#endif

// All GPIO pins support interrupts on ESP32.
#ifndef digitalPinToInterrupt
#define digitalPinToInterrupt(p) (p)
#endif

// Port number: 0 for pins 0-31, 1 for pins 32+.
#ifndef digitalPinToPort
#define digitalPinToPort(p)     ((p) < 32 ? 0 : 1)
#endif

// Bitmask within the 32-bit port register.
#ifndef digitalPinToBitMask
#define digitalPinToBitMask(p)  (1UL << ((p) % 32))
#endif

// Pointer to the 32-bit output register for the given port (0 or 1).
#ifndef portOutputRegister
#define portOutputRegister(port) \
    ((volatile uint32_t *)((port) == 0 ? GPIO_OUT_REG : GPIO_OUT1_REG))
#endif

// Pointer to the 32-bit input register for the given port (0 or 1).
#ifndef portInputRegister
#define portInputRegister(port) \
    ((volatile uint32_t *)((port) == 0 ? GPIO_IN_REG : GPIO_IN1_REG))
#endif

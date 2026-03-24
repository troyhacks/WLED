// esp32-hal-cpu.h shim for WLED IDF builds.
// LovyanGFX Bus_SPI.cpp includes this for getApbFrequency() / getCpuFrequencyMhz().
#pragma once
#include "esp_private/esp_clk.h"

#ifndef getApbFrequency
static inline uint32_t getApbFrequency() {
    return (uint32_t)esp_clk_apb_freq();
}
#endif

#ifndef getCpuFrequencyMhz
static inline uint32_t getCpuFrequencyMhz() {
    return (uint32_t)(esp_clk_cpu_freq() / 1000000);
}
#endif

// arduino_cpu.cpp - the Arduino core's clock helpers.
//
// The core has them in cores/esp32/esp32-hal-cpu.c.  LovyanGFX uses the
// header for the APB frequency and the cycle counter.

#include "Arduino.h"
#include "esp32-hal-cpu.h"

extern "C" {
#include "esp_private/esp_clk.h"
}

bool addApbChangeCallback(void *arg, apb_change_cb_t cb)
{
    (void)arg;
    (void)cb;
    // The core watches the APB clock change; nothing here does.
    return false;
}

bool removeApbChangeCallback(void *arg, apb_change_cb_t cb)
{
    (void)arg;
    (void)cb;
    return false;
}

bool setCpuFrequencyMhz(uint32_t cpu_freq_mhz)
{
    (void)cpu_freq_mhz;
    // The P4 runs at a fixed 360 MHz.
    return false;
}

uint32_t getCpuFrequencyMhz()
{
    return (uint32_t)esp_clk_cpu_freq() / 1000000U;
}

uint32_t getXtalFrequencyMhz()
{
    return 40;
}

uint32_t getApbFrequency()
{
    return (uint32_t)esp_clk_apb_freq();
}

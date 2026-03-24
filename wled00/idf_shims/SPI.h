// SPI.h shim for WLED IDF builds.
// SPI is not actively used in this build target.
// Also provides the Arduino-ESP32 SPI HAL stubs that LovyanGFX common.cpp
// compiles when ARDUINO is defined and <SPI.h> is present.
#pragma once
#include "idf_compat.h"

// ── Arduino-ESP32 SPI HAL stubs ───────────────────────────────────────────────
// LovyanGFX platforms/esp32/common.cpp uses these when ARDUINO is defined.
// All are no-ops in our IDF build (we use the IDF SPI driver directly).
typedef struct { int bus; } spi_t;

#ifndef HSPI
#define HSPI 2
#endif
#ifndef FSPI
#define FSPI 0
#endif

static inline spi_t* spiStartBus(uint8_t /*spiNum*/, uint32_t /*clockDiv*/,
                                  uint8_t /*dataMode*/, uint8_t /*bitOrder*/) {
    return nullptr;
}
static inline void spiStopBus(spi_t* /*spi*/) {}
static inline void spiSimpleTransaction(spi_t* /*spi*/) {}
static inline void spiEndTransaction(spi_t* /*spi*/) {}

// ── Arduino SPIClass shim ─────────────────────────────────────────────────────
struct _WledSPI {
    void begin(int sck=-1, int miso=-1, int mosi=-1, int ss=-1) {}
    void end() {}
    void setBitOrder(uint8_t) {}
    void setDataMode(uint8_t) {}
    void setClockDivider(uint32_t) {}
    void setFrequency(uint32_t) {}
    uint8_t transfer(uint8_t data) { return 0; }
    uint16_t transfer16(uint16_t data) { return 0; }
    void transfer(void* buf, size_t count) {}
    spi_t* bus() { return nullptr; }
};
extern _WledSPI SPI;

// SPI.h - the Arduino SPI class, over ESP-IDF's spi_master driver.
//
// The P4's displays do not use SPI - the HDMI bridge and the MIPI-DSI panels
// have their own drivers - but the Arduino libraries and a few usermods
// still expect the class to be there.

#pragma once

#include "Arduino.h"
#include "Print.h"
#include "Stream.h"

#define MSBFIRST 1
#define LSBFIRST 0

// The SPI bus numbers and modes, the way the Arduino core has them.
#define FSPI 0  // the SPI 2 bus
#define HSPI 1  // the SPI 3 bus
#define VSPI HSPI

#define SPI_MODE0 0
#define SPI_MODE1 1
#define SPI_MODE2 2
#define SPI_MODE3 3

#define SPI_SS0         0
#define SPI_SS1         1
#define SPI_SS2         2
#define SPI_SS_MASK_ALL 0x7

#define SPI_CLOCK_DIV2   0x00101001  // 8 MHz
#define SPI_CLOCK_DIV4   0x00241001  // 4 MHz
#define SPI_CLOCK_DIV8   0x004c1001  // 2 MHz
#define SPI_CLOCK_DIV16  0x009c1001  // 1 MHz
#define SPI_CLOCK_DIV32  0x013c1001  // 500 kHz
#define SPI_CLOCK_DIV64  0x027c1001  // 250 kHz
#define SPI_CLOCK_DIV128 0x04fc1001  // 125 kHz

#define SPI_HAS_TRANSACTION 1

class SPISettings {
public:
    SPISettings(uint32_t clock, uint8_t bitOrder, uint8_t dataMode) {
        _clock = clock;
        _bitOrder = bitOrder;
        _dataMode = dataMode;
    }
    SPISettings() : _clock(4000000), _bitOrder(MSBFIRST), _dataMode(0) {}

    uint32_t clock() const { return _clock; }
    uint8_t bitOrder() const { return _bitOrder; }
    uint8_t dataMode() const { return _dataMode; }

private:
    uint32_t _clock;
    uint8_t _bitOrder;
    uint8_t _dataMode;
};

class SPIClass : public Stream {
public:
    SPIClass(int8_t sck = -1, int8_t miso = -1, int8_t mosi = -1, int8_t ss = -1);
    ~SPIClass() override;

    void begin(int8_t sck = -1, int8_t miso = -1, int8_t mosi = -1, int8_t ss = -1);
    void end();
    void beginTransaction(SPISettings settings);
    void endTransaction();

    uint8_t transfer(uint8_t data);
    void transfer(void *buffer, size_t size);
    uint8_t transfer(uint8_t data, uint8_t sendBits);
    uint8_t transfer(const uint8_t *data, size_t size);

    size_t write(uint8_t c) override;
    size_t write(const uint8_t *buffer, size_t size) override;
    size_t writeBytes(const uint8_t *buffer, size_t size);
    int available() override;
    int read() override;
    int peek() override;
    void flush() override;

    void setFrequency(uint32_t clock);
    void setBitOrder(uint8_t bitOrder);
    void setDataMode(uint8_t dataMode);
    void setClock(int32_t clock) { setFrequency((uint32_t)clock); }

    // The Arduino core's old driver's bus handle.  LovyanGFX asks for it.
    spi_t *bus();

private:
    int8_t _sck, _miso, _mosi, _ss;
    bool _begun = false;
    uint32_t _clock = 4000000;
    uint8_t _bitOrder = MSBFIRST;
    uint8_t _dataMode = 0;
    void * _handle = nullptr;

    // The transfer itself: the bytes go out and the bytes that come back
    // (if any) are put where the caller asked for them.
    void transfer(const uint8_t *data, size_t size, uint8_t *received);
};

extern SPIClass SPI;

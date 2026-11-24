// Wire.h - the Arduino I2C class, over ESP-IDF's i2c_master driver.
//
// One I2C bus per pin pair, and the bus belongs to the board: the display
// bridge, the I2C scan and the audio codec all sit on it.  So this Wire does
// not own a bus - it joins the board's, which is what the registry in
// arduino_wire.cpp hands out.

#pragma once

#include "Arduino.h"
#include "Print.h"
#include "Stream.h"

extern "C" {
#include "driver/i2c_master.h"
}

// The board's shared I2C buses, one per pin pair, installed on demand and
// never deleted.  There must not be two buses on the same pins: the IDF's
// i2c_master driver gives the GPIO routing to the last controller that
// installs itself, and the other one's transactions then NACK forever.
i2c_master_bus_handle_t wledI2cBusEnsure(int8_t sda, int8_t scl);
i2c_port_t wledI2cBusPort(i2c_master_bus_handle_t bus);

class TwoWire : public Stream {
public:
    void begin(int8_t sda = -1, int8_t scl = -1, uint32_t frequency = 0);
    void begin(uint8_t address);
    void end();
    void setClock(uint32_t frequency);
    void setPins(int8_t sda, int8_t scl, bool pullup = true);
    void setTimeOut(uint16_t timeout);

    void beginTransmission(uint8_t address);
    size_t write(uint8_t data);
    size_t write(const uint8_t *data, size_t length);
    uint8_t endTransmission(bool sendStop = true);

    uint8_t requestFrom(uint8_t address, size_t size, bool sendStop = true);
    uint8_t requestFrom(uint8_t address, size_t size);

    int available() override;
    int read() override;
    int peek() override;

private:
    bool ensureBus();
    i2c_master_bus_handle_t _bus = nullptr;
    i2c_master_dev_handle_t _dev = nullptr;
    uint8_t _address = 0;
    uint32_t _frequency = 0;
    uint16_t _timeoutMs = 50;
    uint8_t _rxBuffer[256];
    size_t _rxLength = 0;
    size_t _rxIndex = 0;
};

extern TwoWire Wire;

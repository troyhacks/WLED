// arduino_wire.cpp - the Arduino I2C class, over ESP-IDF's i2c_master driver.
//
// This file also owns the board's shared I2C buses.  There is exactly one bus
// per pin pair and it is never deleted: the display bridge, the I2C scan and
// the audio codec all sit on it, and the last controller to install itself
// takes the GPIO routing away from the others.
//
// WLED's own globalI2cBusEnsure() (wled00/util.cpp) forwards to
// wledI2cBusEnsure() here, so the Wire class and the rest of WLED share one
// bus instead of fighting over the pins.

#include "Arduino.h"
#include "Wire.h"

extern "C" {
#include "driver/i2c_master.h"
#include "soc/soc_caps.h"
#include "esp_log.h"
}

static const char *TAG = "wled_i2c";

namespace {

constexpr int MAX_BUSES = 3;

struct BusSlot {
    int8_t sda = -1;
    int8_t scl = -1;
    i2c_port_t port = I2C_NUM_0;
    i2c_master_bus_handle_t handle = nullptr;
};

BusSlot s_buses[MAX_BUSES];
uint8_t s_busCount = 0;

} // namespace

i2c_master_bus_handle_t wledI2cBusEnsure(int8_t sda, int8_t scl)
{
    if (sda < 0 || scl < 0) {
        return nullptr;
    }

    for (uint8_t i = 0; i < s_busCount; i++) {
        if (s_buses[i].sda == sda && s_buses[i].scl == scl) {
            return s_buses[i].handle;
        }
    }
    if (s_busCount >= MAX_BUSES) {
        return nullptr;
    }

    // Which controller is free?  The IDF allows one bus per port.  Port 1 is
    // preferred - the display bridge and the I2C scan have always lived there
    // - and it leaves port 0, Wire's own port, alone.
    i2c_port_t order[2];
    uint8_t tries = 0;
#if SOC_HP_I2C_NUM > 1
    order[tries++] = (i2c_port_t)1;
#endif
    order[tries++] = (i2c_port_t)0;

    i2c_master_bus_handle_t bus = nullptr;
    i2c_port_t port = I2C_NUM_0;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    for (uint8_t p = 0; p < tries; p++) {
        i2c_master_bus_config_t busCfg = {};
        busCfg.i2c_port = order[p];
        busCfg.sda_io_num = (gpio_num_t)sda;
        busCfg.scl_io_num = (gpio_num_t)scl;
        busCfg.clk_source = I2C_CLK_SRC_DEFAULT;
        busCfg.glitch_ignore_cnt = 7;
        busCfg.flags.enable_internal_pullup = true;
        busCfg.flags.allow_pd = false;

        err = i2c_new_master_bus(&busCfg, &bus);
        if (err == ESP_OK && bus != nullptr) {
            port = order[p];
            break;
        }
        bus = nullptr; // this port is taken - try the next one
    }
    if (bus == nullptr) {
        ESP_LOGW(TAG, "no bus for SDA=%d SCL=%d (err %d)", sda, scl, (int)err);
        return nullptr;
    }

    BusSlot &slot = s_buses[s_busCount++];
    slot.sda = sda;
    slot.scl = scl;
    slot.port = port;
    slot.handle = bus;
    ESP_LOGI(TAG, "bus installed on port %d, SDA=%d SCL=%d", (int)port, sda, scl);
    return bus;
}

i2c_port_t wledI2cBusPort(i2c_master_bus_handle_t bus)
{
    for (uint8_t i = 0; i < s_busCount; i++) {
        if (s_buses[i].handle == bus) {
            return s_buses[i].port;
        }
    }
    return I2C_NUM_0;
}

// --- the Wire class ---------------------------------------------------------

TwoWire Wire;

bool TwoWire::begin(int sda, int scl, uint32_t frequency)
{
    if (sda >= 0 && scl >= 0) {
        if (!setPins(sda, scl)) {
            return false;
        }
    }
    if (frequency) {
        setClock(frequency);
    }
    return ensureBus();
}

bool TwoWire::begin(uint8_t address)
{
    _address = address;
    return ensureBus();
}

void TwoWire::end()
{
    // The bus belongs to the board, so it stays.  Only this class's device
    // handle is released.
    if (_dev && _bus) {
        i2c_master_bus_rm_device(_dev);
        _dev = nullptr;
    }
    _bus = nullptr;
}

bool TwoWire::setClock(uint32_t frequency)
{
    _frequency = frequency;
    return true;
}

bool TwoWire::setPins(int sda, int scl, bool pullup)
{
    (void)pullup;
    _bus = wledI2cBusEnsure(sda, scl);
    _dev = nullptr;
    return _bus != nullptr;
}

void TwoWire::setTimeOut(uint16_t timeout)
{
    _timeoutMs = timeout;
}

bool TwoWire::ensureBus()
{
    if (_bus && _dev) {
        return true;
    }
    if (!_bus) {
        // Without pins there is nothing to join; the Arduino core would use
        // its default pins here.
        return false;
    }
    i2c_device_config_t devCfg = {};
    devCfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    devCfg.device_address = _address;
    devCfg.scl_speed_hz = _frequency ? _frequency : 100000;
    return i2c_master_bus_add_device(_bus, &devCfg, &_dev) == ESP_OK;
}

void TwoWire::beginTransmission(uint8_t address)
{
    _address = address;
    _rxLength = 0;
    _rxIndex = 0;
    if (_dev) {
        i2c_master_bus_rm_device(_dev);
        _dev = nullptr;
    }
    ensureBus();
}

size_t TwoWire::write(uint8_t data)
{
    return write(&data, 1);
}

size_t TwoWire::write(const uint8_t *data, size_t length)
{
    if (!_dev || !data || !length) {
        return 0;
    }
    if (i2c_master_transmit(_dev, data, length, _timeoutMs) != ESP_OK) {
        return 0;
    }
    return length;
}

uint8_t TwoWire::endTransmission(bool sendStop)
{
    (void)sendStop;
    // The IDF's master driver has no separate stop: the transaction always
    // ends with one.
    return 0;
}

uint8_t TwoWire::endTransmission()
{
    return endTransmission(true);
}

size_t TwoWire::requestFrom(uint8_t address, size_t size, bool sendStop)
{
    (void)sendStop;
    if (size > sizeof(_rxBuffer)) {
        size = sizeof(_rxBuffer);
    }
    _address = address;
    _rxLength = 0;
    _rxIndex = 0;
    if (!_dev) {
        if (_bus) {
            i2c_master_bus_rm_device(_dev);
            _dev = nullptr;
        }
        ensureBus();
    }
    if (!_dev) {
        return 0;
    }
    if (i2c_master_receive(_dev, _rxBuffer, size, _timeoutMs) != ESP_OK) {
        return 0;
    }
    _rxLength = size;
    return size;
}

size_t TwoWire::requestFrom(uint8_t address, size_t size)
{
    return requestFrom(address, size, true);
}

int TwoWire::available()
{
    return (int)(_rxLength - _rxIndex);
}

int TwoWire::read()
{
    if (_rxIndex >= _rxLength) {
        return -1;
    }
    return (int)_rxBuffer[_rxIndex++];
}

int TwoWire::peek()
{
    if (_rxIndex >= _rxLength) {
        return -1;
    }
    return (int)_rxBuffer[_rxIndex];
}

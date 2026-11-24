// arduino_spi.cpp - the Arduino SPI class, over ESP-IDF's spi_master driver.

#include "Arduino.h"
#include "SPI.h"

extern "C" {
#include "driver/spi_master.h"
#include "esp_log.h"
}

SPIClass SPI;

SPIClass::SPIClass(int8_t sck, int8_t miso, int8_t mosi, int8_t ss)
    : _sck(sck), _miso(miso), _mosi(mosi), _ss(ss)
{
}

SPIClass::~SPIClass()
{
    end();
}

void SPIClass::begin(int8_t sck, int8_t miso, int8_t mosi, int8_t ss)
{
    if (sck >= 0) {
        _sck = sck;
    }
    if (miso >= 0) {
        _miso = miso;
    }
    if (mosi >= 0) {
        _mosi = mosi;
    }
    if (ss >= 0) {
        _ss = ss;
    }
    if (_begun) {
        return;
    }
    // The bus is installed on whichever host the pins are on; the P4 has one
    // SPI2 with GPIO 33..36 on the free pins.
    spi_bus_config_t busCfg = {};
    busCfg.mosi_io_num = _mosi;
    busCfg.miso_io_num = _miso;
    busCfg.sclk_io_num = _sck;
    busCfg.quadwp_io_num = -1;
    busCfg.quadhd_io_num = -1;
    busCfg.max_transfer_sz = 4096;

    if (spi_bus_initialize(SPI2_HOST, &busCfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        // The bus may already be up - that is not an error for us.
    }

    spi_device_interface_config_t devCfg = {};
    devCfg.mode = _dataMode;
    devCfg.clock_speed_hz = (int)_clock;
    devCfg.spics_io_num = _ss;
    devCfg.queue_size = 7;
    devCfg.flags = _bitOrder == LSBFIRST ? SPI_DEVICE_HALFDUPLEX : 0;

    spi_device_handle_t handle = nullptr;
    if (spi_bus_add_device(SPI2_HOST, &devCfg, &handle) == ESP_OK) {
        _handle = handle;
        _begun = true;
    }
}

void SPIClass::end()
{
    if (_handle) {
        spi_bus_remove_device((spi_device_handle_t)_handle);
        _handle = nullptr;
    }
    _begun = false;
}

void SPIClass::beginTransaction(SPISettings settings)
{
    _clock = settings.clock();
    _bitOrder = settings.bitOrder();
    _dataMode = settings.dataMode();
    if (!_begun) {
        begin();
    } else {
        spi_device_t *dev = (spi_device_t *)_handle;
        dev->cfg.clock_speed_hz = (int)_clock;
        dev->cfg.mode = _dataMode;
    }
}

void SPIClass::endTransaction()
{
}

uint8_t SPIClass::transfer(uint8_t data)
{
    uint8_t received = 0;
    transfer(&data, 1, &received);
    return received;
}

void SPIClass::transfer(void *buffer, size_t size)
{
    transfer((const uint8_t *)buffer, size, nullptr);
}

uint8_t SPIClass::transfer(uint8_t data, uint8_t sendBits)
{
    (void)sendBits;
    return transfer(data);
}

uint8_t SPIClass::transfer(const uint8_t *data, size_t size)
{
    uint8_t received = 0;
    transfer(data, size, &received);
    return received;
}

void SPIClass::transfer(const uint8_t *data, size_t size, uint8_t *received)
{
    if (!_begun || !data || !size) {
        return;
    }
    uint8_t *out = (uint8_t *)malloc(size);
    if (!out) {
        return;
    }
    memcpy(out, data, size);
    spi_transaction_t trans = {};
    trans.length = size * 8;
    trans.tx_buffer = out;
    trans.rx_buffer = received;
    spi_device_polling_transmit((spi_device_handle_t)_handle, &trans);
    free(out);
}

size_t SPIClass::write(uint8_t c)
{
    return write(&c, 1);
}

size_t SPIClass::write(const uint8_t *buffer, size_t size)
{
    if (!_begun || !buffer || !size) {
        return 0;
    }
    uint8_t *out = (uint8_t *)malloc(size);
    if (!out) {
        return 0;
    }
    memcpy(out, buffer, size);
    spi_transaction_t trans = {};
    trans.length = size * 8;
    trans.tx_buffer = out;
    spi_device_polling_transmit((spi_device_handle_t)_handle, &trans);
    free(out);
    return size;
}

int SPIClass::available()
{
    return 0;
}

size_t SPIClass::writeBytes(const uint8_t *buffer, size_t size)
{
    return write(buffer, size);
}

int SPIClass::read()
{
    return -1;
}

int SPIClass::peek()
{
    return -1;
}

void SPIClass::flush()
{
}

void SPIClass::setFrequency(uint32_t clock)
{
    _clock = clock;
}

void SPIClass::setBitOrder(uint8_t bitOrder)
{
    _bitOrder = bitOrder;
}

void SPIClass::setDataMode(uint8_t dataMode)
{
    _dataMode = dataMode;
}

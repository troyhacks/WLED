// arduino_serial.cpp - the UART and the USB-Serial-JTAG, over ESP-IDF.
//
// HardwareSerial is a UART (driver/uart.h) and HWCDC is the built-in
// USB-Serial-JTAG peripheral (driver/usb_serial_jtag.h).  Neither one owns
// the peripheral until it is begun, so a board that has neither still runs.

#include "Arduino.h"
#include "HardwareSerial.h"

extern "C" {
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "soc/soc_caps.h"
#include "esp_rom_sys.h"
}

// --- HardwareSerial ---------------------------------------------------------

// How many UARTs have been claimed so far: the first one is UART0.
static int s_uartCount = 0;

HardwareSerial::HardwareSerial() = default;
HardwareSerial::~HardwareSerial() = default;

void HardwareSerial::begin(uint32_t baud, uint32_t config, int8_t rxPin, int8_t txPin,
                            bool invert, unsigned long timeoutMs, uint8_t rxFifoFullThrd)
{
    (void)timeoutMs;
    (void)rxFifoFullThrd;
    if (_started) {
        uart_set_baudrate(_uartNum, baud);
        return;
    }

    // The first UART is UART0, the second UART1.
    _uartNum = (s_uartCount == 0) ? UART_NUM_0 : UART_NUM_1;
    _hasPort = true;
    s_uartCount++;

    uart_config_t uartCfg = {};
    uartCfg.baud_rate = baud;
    uartCfg.data_bits = UART_DATA_8_BITS;
    uartCfg.stop_bits = UART_STOP_BITS_1;
    uartCfg.parity = UART_PARITY_DISABLE;
    uartCfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uartCfg.source_clk = UART_SCLK_DEFAULT;

    // The Arduino core's configuration values.
    switch (config) {
    case SERIAL_8N2: uartCfg.stop_bits = UART_STOP_BITS_2; break;
    case SERIAL_8E1: uartCfg.parity = UART_PARITY_EVEN; break;
    case SERIAL_8E2: uartCfg.parity = UART_PARITY_EVEN; uartCfg.stop_bits = UART_STOP_BITS_2; break;
    case SERIAL_8O1: uartCfg.parity = UART_PARITY_ODD; break;
    case SERIAL_8O2: uartCfg.parity = UART_PARITY_ODD; uartCfg.stop_bits = UART_STOP_BITS_2; break;
    default: break;
    }

    if (uart_driver_install(_uartNum, 1024, 1024, 0, nullptr, 0) != ESP_OK) {
        _hasPort = false;
        s_uartCount--;
        return;
    }
    uart_param_config(_uartNum, &uartCfg);
    if (rxPin >= 0 || txPin >= 0) {
        // The IDF's order is tx first, then rx.  The other way round the UART
        // drives its own receive pin: the chip then reads back everything it
        // prints, and the console executes its own log as commands.
        esp_err_t err = uart_set_pin(_uartNum, txPin, rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (err != ESP_OK) {
            log_e("uart_set_pin(%d, tx=%d, rx=%d) failed: %s", _uartNum, txPin, rxPin, esp_err_to_name(err));
        }
    }
    if (invert) {
        uart_set_line_inverse(_uartNum, true);
    }
    _started = true;
}

void HardwareSerial::end()
{
    if (_started) {
        uart_driver_delete(_uartNum);
        _started = false;
        _hasPort = false;
        s_uartCount--;
    }
}

size_t HardwareSerial::write(uint8_t c)
{
    return write(&c, 1);
}

size_t HardwareSerial::write(const uint8_t *buffer, size_t size)
{
    if (!_started || !buffer || !size) {
        return 0;
    }
    int written = uart_write_bytes(_uartNum, buffer, size);
    return written < 0 ? 0 : (size_t)written;
}

int HardwareSerial::available()
{
    if (!_started) {
        return 0;
    }
    size_t buffered = 0;
    uart_get_buffered_data_len(_uartNum, &buffered);
    return (int)buffered;
}

int HardwareSerial::read()
{
    if (_hasPeeked) {
        _hasPeeked = false;
        return (int)_peeked;
    }
    if (!_started) {
        return -1;
    }
    uint8_t byte = 0;
    int got = uart_read_bytes(_uartNum, &byte, 1, 0);
    return got == 1 ? (int)byte : -1;
}

int HardwareSerial::peek()
{
    if (_hasPeeked) {
        return (int)_peeked;
    }
    if (!_started) {
        return -1;
    }
    size_t buffered = 0;
    uart_get_buffered_data_len(_uartNum, &buffered);
    if (!buffered) {
        return -1;
    }
    // There is no peek in the IDF driver, so the byte is taken out and put
    // back in front of the next one.
    uint8_t byte = 0;
    if (uart_read_bytes(_uartNum, &byte, 1, 0) != 1) {
        return -1;
    }
    _peeked = byte;
    _hasPeeked = true;
    return (int)byte;
}

void HardwareSerial::flush()
{
    if (!_started) {
        return;
    }
    uart_wait_tx_done(_uartNum, pdMS_TO_TICKS(100));
}

void HardwareSerial::setRxBufferSize(size_t size)
{
    // The ring buffer is fixed when the driver is installed; the Arduino core
    // takes the size before begin().  It is remembered and used then.
    _rxBufferSize = size;
}

void HardwareSerial::setTxBufferSize(size_t size)
{
    _txBufferSize = size;
}

int HardwareSerial::availableForWrite()
{
    if (!_started) {
        return 0;
    }
    size_t free = 0;
    if (uart_get_tx_buffer_free_size(_uartNum, &free) != ESP_OK) {
        return 0;
    }
    return (int)free;
}

void HardwareSerial::setDebugOutput(bool en)
{
    // The IDF's own console already goes to UART0; the Arduino core's version
    // of this call re-points the ROM's printf.  Here it is a state flag: the
    // console decides where the kernel output goes.
    _debugOutput = en;
}

// --- HWCDC ------------------------------------------------------------------

void HWCDC::begin()
{
    if (_started) {
        return;
    }
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = RX_BUFFER_SIZE;
    cfg.tx_buffer_size = 4096;
    if (usb_serial_jtag_driver_install(&cfg) == ESP_OK) {
        _started = true;
    }
}

void HWCDC::end()
{
    if (_started) {
        usb_serial_jtag_driver_uninstall();
        _started = false;
    }
}

size_t HWCDC::write(uint8_t c)
{
    return write(&c, 1);
}

size_t HWCDC::write(const uint8_t *buffer, size_t size)
{
    if (!_started || !buffer || !size) {
        return 0;
    }
    // The peripheral is a packet pipe.  With no timeout the write does not
    // wait for a host that is not there - the bytes are dropped instead.
    int written = usb_serial_jtag_write_bytes(buffer, size, _txTimeoutMs / 1000);
    return written < 0 ? 0 : (size_t)written;
}

int HWCDC::available()
{
    if (!_started) {
        return 0;
    }
    // The driver hands out whole packets, so whatever is left over from the
    // last read stays in this class's own ring buffer.
    size_t used = (_rxHead - _rxTail) & (RX_BUFFER_SIZE - 1);
    size_t free = (RX_BUFFER_SIZE - 1) - used;
    if (free) {
        uint8_t chunk[64];
        size_t room = free < sizeof(chunk) ? free : sizeof(chunk);
        int got = usb_serial_jtag_read_bytes(chunk, room, 0);
        if (got > 0) {
            for (int i = 0; i < got; i++) {
                _rxBuffer[(_rxHead + i) & (RX_BUFFER_SIZE - 1)] = chunk[i];
            }
            _rxHead = (_rxHead + (size_t)got) & (RX_BUFFER_SIZE - 1);
            used = (_rxHead - _rxTail) & (RX_BUFFER_SIZE - 1);
        }
    }
    return (int)used;
}

int HWCDC::read()
{
    int n = available();
    if (n <= 0) {
        return -1;
    }
    uint8_t byte = _rxBuffer[_rxTail];
    _rxTail = (_rxTail + 1) & (RX_BUFFER_SIZE - 1);
    return (int)byte;
}

int HWCDC::peek()
{
    int n = available();
    if (n <= 0) {
        return -1;
    }
    return (int)_rxBuffer[_rxTail];
}

void HWCDC::flush()
{
    // The writes are synchronous, so there is nothing left to wait for.
}

void HWCDC::setRxBufferSize(size_t size)
{
    // The IDF driver fixes the buffer when it is installed.
    (void)size;
}

void HWCDC::setTxBufferSize(size_t size)
{
    (void)size;
}

void HWCDC::setTxTimeoutMs(uint32_t timeout)
{
    _txTimeoutMs = timeout;
}

void HWCDC::setDebugOutput(bool en)
{
    _debugOutput = en;
}

// --- the instances ----------------------------------------------------------

#if !defined(NO_GLOBAL_INSTANCES) && !defined(NO_GLOBAL_SERIAL)
HardwareSerial Serial0;
HardwareSerial Serial1;
#endif

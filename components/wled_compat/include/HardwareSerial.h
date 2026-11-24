// HardwareSerial.h - the Arduino serial classes, backed by ESP-IDF.
//
// Two classes live here:
//
//   HardwareSerial  - a UART, over driver/uart.h
//   HWCDC           - the built-in USB-Serial-JTAG, over driver/usb_serial_jtag.h
//
// The Arduino core owns the peripherals and the console; here the firmware
// owns them.  WLED's own console (wled00/wled_console.h) sits on top of these
// two and writes to whichever of them is there.

#pragma once

#include "Arduino.h"
#include "Print.h"
#include "Stream.h"

extern "C" {
#include "driver/uart.h"
}

// The serial configuration constants, the way the IDF spells them - the
// Arduino core's values are the same bits in a different order.
#define SERIAL_8N1 (UART_DATA_8_BITS | UART_STOP_BITS_1 | UART_PARITY_DISABLE)
#define SERIAL_8N2 (UART_DATA_8_BITS | UART_STOP_BITS_2 | UART_PARITY_DISABLE)
#define SERIAL_8E1 (UART_DATA_8_BITS | UART_STOP_BITS_1 | UART_PARITY_EVEN)
#define SERIAL_8E2 (UART_DATA_8_BITS | UART_STOP_BITS_2 | UART_PARITY_EVEN)
#define SERIAL_8O1 (UART_DATA_8_BITS | UART_STOP_BITS_1 | UART_PARITY_ODD)
#define SERIAL_8O2 (UART_DATA_8_BITS | UART_STOP_BITS_2 | UART_PARITY_ODD)

// UART0's pins are fixed by the bootloader.  The Arduino core has the table
// for every SoC; here is the P4's.
#ifndef SOC_RX0
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#define SOC_RX0 (gpio_num_t)38
#endif
#endif
#ifndef SOC_TX0
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#define SOC_TX0 (gpio_num_t)37
#endif
#endif

class HardwareSerial : public Stream {
public:
    HardwareSerial();
    ~HardwareSerial() override;

    void begin(uint32_t baud, uint32_t config = SERIAL_8N1, int8_t rxPin = -1,
               int8_t txPin = -1, bool invert = false, unsigned long timeoutMs = 20000UL,
               uint8_t rxFifoFullThrd = 112);
    void end();

    size_t write(uint8_t c) override;
    size_t write(const uint8_t *buffer, size_t size) override;
    int available() override;
    int read() override;
    int peek() override;
    void flush() override;

    void setRxBufferSize(size_t size);
    void setTxBufferSize(size_t size);
    int availableForWrite() override;
    void setDebugOutput(bool en);
    operator bool() const { return _started; }

private:
    int _uartNum = -1;
    bool _started = false;
    bool _debugOutput = false;
    size_t _rxBufferSize = 1024;
    size_t _txBufferSize = 1024;
    // The IDF driver has no peek, so a peeked byte is kept here.
    uint8_t _peeked = 0;
    bool _hasPeeked = false;
};

// The USB-Serial-JTAG peripheral lives in its own header, the way the
// Arduino core has it.
#include "HWCDC.h"

#if !defined(NO_GLOBAL_INSTANCES) && !defined(NO_GLOBAL_SERIAL)
extern HardwareSerial Serial0;
extern HardwareSerial Serial1;
#endif

// The Arduino core's "Serial" is a macro, so that WLED's console can take the
// name over (see wled00/wled_console.h).  Here "Serial" is UART0 and the USB
// is reached through HWCDC.
#define Serial Serial0

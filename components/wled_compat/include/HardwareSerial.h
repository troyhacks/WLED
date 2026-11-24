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

// The serial configuration constants the Arduino core defines.
#define SERIAL_8N1 0x800001c
#define SERIAL_8N2 0x800001d
#define SERIAL_8E1 0x800001a
#define SERIAL_8E2 0x800001b
#define SERIAL_8O1 0x800001e
#define SERIAL_8O2 0x800001f

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
    size_t availableForWrite();
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

// The USB-Serial-JTAG peripheral, which is a packet pipe and not a UART: there
// is no baud rate, and the buffers are fixed by the driver.
class HWCDC : public Stream {
public:
    void begin();
    void end();

    size_t write(uint8_t c) override;
    size_t write(const uint8_t *buffer, size_t size) override;
    int available() override;
    int read() override;
    int peek() override;
    void flush() override;

    void setRxBufferSize(size_t size);
    void setTxBufferSize(size_t size);
    void setTxTimeoutMs(uint32_t timeout);
    void setDebugOutput(bool en);
    operator bool() const { return _started; }

private:
    bool _started = false;
    uint32_t _txTimeoutMs = 0;
    bool _debugOutput = false;
    // The peripheral hands us whole packets, so the bytes wait here until the
    // application takes them.
    static constexpr size_t RX_BUFFER_SIZE = 1024;
    uint8_t _rxBuffer[RX_BUFFER_SIZE];
    size_t _rxHead = 0;
    size_t _rxTail = 0;
};

#if !defined(NO_GLOBAL_INSTANCES) && !defined(NO_GLOBAL_SERIAL)
extern HardwareSerial Serial0;
extern HardwareSerial Serial1;
#endif

// The Arduino core's "Serial" is a macro, so that WLED's console can take the
// name over (see wled00/wled_console.h).  Here "Serial" is UART0 and the USB
// is reached through HWCDC.
#define Serial Serial0

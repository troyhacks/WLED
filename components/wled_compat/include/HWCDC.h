// HWCDC.h - the USB-Serial-JTAG class.
//
// The Arduino core has this class for the built-in USB peripheral; here it is
// backed by ESP-IDF's USB-Serial-JTAG driver.  The peripheral is a packet
// pipe, not a UART: there is no baud rate, and the buffers are fixed by the
// driver.

#pragma once

#include "Arduino.h"
#include "Print.h"
#include "Stream.h"

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

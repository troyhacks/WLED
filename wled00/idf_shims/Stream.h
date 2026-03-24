// Stream.h shim for WLED IDF builds.
// LovyanGFX Bus_Stream.hpp includes <Stream.h> to define the Stream base class.
// Provides the subset of Arduino Stream API used by LovyanGFX.
#pragma once
#include "Print.h"

class Stream : public Print {
public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;

    // readBytes: read up to length bytes into buffer, return count read
    size_t readBytes(uint8_t* buf, size_t length) {
        size_t n = 0;
        while (n < length) {
            int c = read();
            if (c < 0) break;
            buf[n++] = (uint8_t)c;
        }
        return n;
    }
    size_t readBytes(char* buf, size_t length) {
        return readBytes(reinterpret_cast<uint8_t*>(buf), length);
    }
};

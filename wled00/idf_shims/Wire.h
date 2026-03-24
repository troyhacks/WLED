// Wire.h shim for WLED IDF builds.
// The IDF build uses driver/i2c_master.h directly.
// This stub satisfies #include <Wire.h> in files not yet migrated.
#pragma once
#include "idf_compat.h"
#include "driver/i2c_master.h"

// Minimal Wire-compatible stub so code that calls Wire.* still compiles.
// Actual I2C operations should use idf_i2c helpers (see cfg.cpp / set.cpp).
struct _WledWire {
    void begin(int sda=-1, int scl=-1, uint32_t freq=100000) {}
    bool setPins(int sda, int scl) { return true; }
    void beginTransmission(uint8_t addr) {}
    uint8_t endTransmission(bool stop=true) { return 0; }
    uint8_t requestFrom(uint8_t addr, uint8_t len, bool stop=true) { return 0; }
    size_t write(uint8_t b)    { return 0; }
    size_t write(const uint8_t* buf, size_t len) { return 0; }
    int    available()         { return 0; }
    int    read()              { return -1; }
    int    peek()              { return -1; }
    void   flush()             {}
    void   setClock(uint32_t) {}
};
typedef _WledWire TwoWire;
extern _WledWire Wire;
extern _WledWire Wire1;

// EEPROM.h - the Arduino EEPROM class, over ESP-IDF's NVS.
//
// The Arduino core keeps the EEPROM in the NVS partition under the "eeprom"
// namespace, so a board flashed with either build finds its settings in the
// same place.

#pragma once

#include "Arduino.h"

class EEPROMClass {
public:
    bool begin(size_t size);
    void end();
    uint8_t read(int address);
    void write(int address, uint8_t value);
    bool commit();
};

extern EEPROMClass EEPROM;

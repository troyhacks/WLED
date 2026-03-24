// idf_compat.cpp — Definitions for Arduino API shims (IDF build only).
#ifdef WLED_IDF_BUILD
#include "idf_compat.h"
#include "idf_shims/Wire.h"
#include "idf_shims/SPI.h"
#include "idf_shims/LittleFS.h"

_WledSerial Serial;
_WledSerial Serial1;
_WledSerial Serial2;

_WledWire Wire;
_WledWire Wire1;

_WledSPI SPI;

LittleFSClass LittleFS;
_EspClass ESP;

#endif // WLED_IDF_BUILD

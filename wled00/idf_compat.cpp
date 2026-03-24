// idf_compat.cpp — Definitions for Arduino API shims (IDF build only).
#ifdef WLED_IDF_BUILD
#include "idf_compat.h"
#include "idf_shims/IPAddress.h"  // full class needed for Serial.print(IPAddress)
#include "idf_shims/Wire.h"
#include "idf_shims/SPI.h"
#include "idf_shims/LittleFS.h"

_WledSerial Serial;
_WledSerial Serial1;
_WledSerial Serial2;

size_t _WledSerial::print(const IPAddress& ip)   { return printf("%s",   ip.toString().c_str()); }
size_t _WledSerial::println(const IPAddress& ip) { return printf("%s\n", ip.toString().c_str()); }

_WledWire Wire;
_WledWire Wire1;

_WledSPI SPI;

LittleFSClass LittleFS;
_EspClass ESP;

#endif // WLED_IDF_BUILD

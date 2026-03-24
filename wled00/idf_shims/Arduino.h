// Arduino.h shim for WLED IDF builds.
// This file sits in wled00/idf_shims/ which is prepended to the include path
// only for the esp32p4_8MB_idf environment (via -I wled00/idf_shims in build_flags).
// Any header that does #include <Arduino.h> or #include "Arduino.h" gets this
// stub instead of the real framework header.
//
// ARDUINO version is defined globally via -DARDUINO=10819 in build_flags so that
// #if ARDUINO >= 100 guards in third-party libs (Timezone, Toki, etc.) pass before
// they reach this include.
#pragma once
#include "idf_compat.h"

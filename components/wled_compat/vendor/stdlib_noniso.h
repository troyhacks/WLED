// stdlib_noniso.h - the Arduino core's header for newlib's non-ISO
// extensions.
//
// The Arduino core ships this header with the functions the C library does
// not have (strlcpy, strlcat and friends).  ESP-IDF uses the same newlib, and
// those functions are already declared in its <string.h>, so this only has to
// pull them in.
//
// It is here because the vendored WString.cpp includes it.

#pragma once

#include <stdlib.h>
#include <string.h>

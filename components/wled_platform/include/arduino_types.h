// arduino_types.h - the Arduino core's basic types.
//
// The core spells them byte, boolean and word.  They live here, in the
// platform layer, so that the portable classes (String, Print, Stream) and
// the filesystem can use them without reaching into the Arduino layer.
//
// The Arduino layer includes this header, so the names are the same
// everywhere - there is one definition of each.

#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef uint8_t byte;
typedef bool boolean;
typedef unsigned int word;

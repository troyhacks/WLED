// pins_arduino.h - the Arduino pin names.
//
// The Arduino core's pins_arduino.h is a list of #defines for the pins the
// board has.  The P4's numbers come from the SoC's own header
// (soc/soc_caps.h), which is what the code uses; this file only carries the
// names the Arduino libraries ask for.

#pragma once

#include "soc/soc_caps.h"

#ifndef LED_BUILTIN
#define LED_BUILTIN 0
#endif

// The P4's boot LED and the console pins.
#define BUILTIN_LED LED_BUILTIN

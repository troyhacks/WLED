// stdlib_noniso.h - the Arduino core's header for newlib's non-ISO
// extensions.
//
// The Arduino core ships this header with the number conversions that C does
// not have.  ESP-IDF uses the same newlib, and some of them are there, but
// not all of them; the ones that are missing are spelled out here over
// snprintf().
//
// It is here because the vendored Arduino files include it.

#pragma once

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef itoa
char *itoa(int value, char *s, int radix);
#endif
#ifndef ltoa
char *ltoa(long value, char *s, int radix);
#endif
#ifndef lltoa
char *lltoa(long long value, char *s, int radix);
#endif
#ifndef utoa
char *utoa(unsigned int value, char *s, int radix);
#endif
#ifndef ultoa
char *ultoa(unsigned long value, char *s, int radix);
#endif
#ifndef ulltoa
char *ulltoa(unsigned long long value, char *s, int radix);
#endif
#ifndef dtostrf
char *dtostrf(double value, signed int width, unsigned int prec, char *buffer);
#endif

#ifdef __cplusplus
}
#endif

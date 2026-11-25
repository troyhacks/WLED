// arduino_stdlib.cpp - the Arduino core's number conversions.
//
// The core has them in cores/esp32/stdlib_noniso.c.  Here they are over
// snprintf(), which newlib has.  dtostrf() is here too: the core has it in
// the same place, and the String class needs it.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "stdlib_noniso.h"

extern "C" {

static char *convert(long long value, char *s, int radix, bool negative)
{
    if (!s) {
        return s;
    }
    char buffer[72];
    size_t i = 0;
    unsigned long long uvalue = negative ? (unsigned long long)(-(value + 1)) + 1ULL
                                         : (unsigned long long)value;
    if (uvalue == 0) {
        buffer[i++] = '0';
    }
    while (uvalue) {
        int digit = (int)(uvalue % (unsigned)radix);
        uvalue /= (unsigned)radix;
        buffer[i++] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
    }
    if (negative) {
        buffer[i++] = '-';
    }
    buffer[i] = '\0';

    // The result is written back to front.
    size_t j = 0;
    for (size_t k = i; k > 0; k--) {
        s[j++] = buffer[k - 1];
    }
    s[j] = '\0';
    return s;
}

char *itoa(int value, char *s, int radix)
{
    return convert(value, s, radix, value < 0);
}

char *ltoa(long value, char *s, int radix)
{
    return convert(value, s, radix, value < 0);
}

char *lltoa(long long value, char *s, int radix)
{
    return convert(value, s, radix, value < 0);
}

char *utoa(unsigned int value, char *s, int radix)
{
    return convert((long long)value, s, radix, false);
}

char *ultoa(unsigned long value, char *s, int radix)
{
    return convert((long long)value, s, radix, false);
}

char *ulltoa(unsigned long long value, char *s, int radix)
{
    return convert((long long)value, s, radix, false);
}

// The Arduino core's dtostrf(): a double as a fixed point string.
//
// The core's "width" is the MINIMUM number of characters the result takes -
// the number is padded on the left with spaces to reach it - and the buffer
// has to have room for width + 1 of them, and more if the number itself is
// longer.  It is NOT the size of the buffer.
//
// This matters: the core's own callers pass the width they want and give
// the buffer more room.  dtostrf(2.8, 3, 1, buf) is "2.8" and it needs four
// bytes; a snprintf() with the width as the buffer size would write "2." and
// stop.
//
// The rounding is part of the contract too, so the digits are taken the
// core's way and not with a printf(): print(1.999, 2) is "2.00", and
// print(2.8, 1) is "2.8" - snprintf's "%.1f" says "2.9" for that one,
// because 2.85 is not exactly representable.
char *dtostrf(double number, signed int width, unsigned int prec, char *s)
{
    if (!s) {
        return s;
    }
    if (isnan(number)) {
        strcpy(s, "nan");
        return s;
    }
    if (isinf(number)) {
        strcpy(s, "inf");
        return s;
    }

    bool negative = false;
    char *out = s;

    int fillme = width;  // how many cells to fill for the integer part
    if (prec > 0) {
        fillme -= (prec + 1);
    }

    // Handle negative numbers
    if (number < 0.0) {
        negative = true;
        fillme--;
        number = -number;
    }

    // Round correctly so that print(1.999, 2) prints as "2.00"
    double rounding = 2.0;
    for (unsigned int i = 0; i < prec; ++i) {
        rounding *= 10.0;
    }
    rounding = 1.0 / rounding;

    number += rounding;

    // Figure out how big our number really is
    double tenpow = 1.0;
    unsigned int digitcount = 1;
    while (number >= 10.0 * tenpow) {
        tenpow *= 10.0;
        digitcount++;
    }

    number /= tenpow;
    fillme -= digitcount;

    // Pad unused cells with spaces
    while (fillme-- > 0) {
        *out++ = ' ';
    }

    // Handle negative sign
    if (negative) {
        *out++ = '-';
    }

    // Print the digits, and if necessary, the decimal point
    digitcount += prec;
    int8_t digit = 0;
    while (digitcount-- > 0) {
        digit = (int8_t)number;
        if (digit > 9) {
            digit = 9;  // insurance
        }
        *out++ = (char)('0' | digit);
        if ((digitcount == prec) && (prec > 0)) {
            *out++ = '.';
        }
        number -= digit;
        number *= 10.0;
    }

    // make sure the string is terminated
    *out = 0;
    return s;
}

} // extern "C"

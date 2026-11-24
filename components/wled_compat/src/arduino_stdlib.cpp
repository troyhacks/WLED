// arduino_stdlib.cpp - the Arduino core's number conversions.
//
// The core has them in cores/esp32/stdlib_noniso.c.  Here they are over
// snprintf(), which newlib has.

#include "Arduino.h"
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

} // extern "C"

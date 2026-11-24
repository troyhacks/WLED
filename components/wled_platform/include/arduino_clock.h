// arduino_clock.h - the Arduino core's clock.
//
// The core's millis(), micros(), delay() and friends.  Here they are
// ESP-IDF's esp_timer and FreeRTOS - which is what the Arduino core uses
// on the P4 as well, so the timings are the same.
//
// The Arduino layer includes this header too, so the libraries that expect
// the core's clock find it exactly where they expect it.

#pragma once

#include <stdint.h>

unsigned long millis();
unsigned long micros();
void delay(unsigned long ms);
void delayMicroseconds(unsigned int us);
void yield();

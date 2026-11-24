// wled_time.cpp - the Arduino clock, over ESP-IDF's esp_timer.
//
// millis() and micros() come straight from the timer; delay() parks the task
// in the FreeRTOS idle queue, which is what the Arduino core does too.

#include "arduino_clock.h"

extern "C" {
#include "esp_timer.h"
#include "esp_rom_sys.h"
}
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

unsigned long millis()
{
    return (unsigned long)(esp_timer_get_time() / 1000LL);
}

unsigned long micros()
{
    return (unsigned long)esp_timer_get_time();
}

void delay(unsigned long ms)
{
    if (ms == 0) {
        yield();
        return;
    }
    // A long delay is split up so that a task that is cancelled in between
    // does not have to wait for the whole period.
    const unsigned long step = 1000;
    while (ms >= step) {
        vTaskDelay(pdMS_TO_TICKS(step));
        ms -= step;
    }
    if (ms) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

void delayMicroseconds(unsigned int us)
{
    // esp_rom_delay_us is a busy wait and burns the CPU; for the short waits
    // WLED asks for that is the right trade (the alternative - a FreeRTOS
    // tick - would be 1 ms at best).
    esp_rom_delay_us(us);
}

void yield()
{
    vTaskDelay(1);
}

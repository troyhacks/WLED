// arduino_pins.cpp - the Arduino core's pin mapping helpers.
//
// The Arduino core has these in esp32-hal-gpio.c and esp32-hal-adc.c.  They
// answer "which ADC channel is this pin?", "which bit of the port?" and so
// on, and WLED's pin manager and the Arduino libraries ask.
//
// The P4's ADC channels:
//   ADC1: GPIO 16..23 -> channels 0..7
//   ADC2: GPIO 49..54 -> channels 0..5
// The Arduino core numbers the channels across the two units, so ADC2's
// channels are 8..13.

#include "Arduino.h"
#include "esp32-hal.h"

namespace {

// The ADC1 pins and the ADC2 pins, in channel order.
const uint8_t ADC1_PINS[] = {16, 17, 18, 19, 20, 21, 22, 23};
const uint8_t ADC2_PINS[] = {49, 50, 51, 52, 53, 54};

constexpr uint8_t ADC1_CHANNELS = sizeof(ADC1_PINS) / sizeof(ADC1_PINS[0]);
constexpr uint8_t ADC2_CHANNELS = sizeof(ADC2_PINS) / sizeof(ADC2_PINS[0]);

} // namespace

int8_t digitalPinToAnalogChannel(uint8_t pin)
{
    for (uint8_t i = 0; i < ADC1_CHANNELS; i++) {
        if (ADC1_PINS[i] == pin) {
            return (int8_t)i;
        }
    }
    for (uint8_t i = 0; i < ADC2_CHANNELS; i++) {
        if (ADC2_PINS[i] == pin) {
            return (int8_t)(ADC1_CHANNELS + i);
        }
    }
    return -1;
}

int8_t analogChannelToDigitalPin(uint8_t channel)
{
    if (channel < ADC1_CHANNELS) {
        return (int8_t)ADC1_PINS[channel];
    }
    channel -= ADC1_CHANNELS;
    if (channel < ADC2_CHANNELS) {
        return (int8_t)ADC2_PINS[channel];
    }
    return -1;
}

int8_t digitalPinToTouchChannel(uint8_t pin)
{
    // The P4 has no touch sensor peripheral - the touch panel on the EV board
    // is a separate controller on the I2C bus.
    (void)pin;
    return -1;
}

uint8_t digitalPinToPort(uint8_t pin)
{
    // The P4 has one GPIO port.
    (void)pin;
    return 0;
}

uint32_t digitalPinToBitMask(uint8_t pin)
{
    if (pin >= SOC_GPIO_PIN_COUNT) {
        return 0;
    }
    return 1UL << pin;
}

uint8_t digitalPinToInterrupt(uint8_t pin)
{
    // Every GPIO on the P4 can be an interrupt source.
    return pin;
}

const char *digitalPinToPinName(uint8_t pin)
{
    static char name[8];
    snprintf(name, sizeof(name), "%u", (unsigned)pin);
    return name;
}

// xTaskCreateUniversal - the Arduino core's task creator, which picks the
// core and puts the task on the stack the core asked for.
BaseType_t xTaskCreateUniversal(TaskFunction_t function, const char *name, uint32_t stackDepth,
                                 void *parameters, UBaseType_t priority, TaskHandle_t *handle,
                                 BaseType_t core)
{
    if (core < 0 || core >= (BaseType_t)portNUM_PROCESSORS) {
        return xTaskCreate(function, name, stackDepth, parameters, priority, handle);
    }
    return xTaskCreatePinnedToCore(function, name, stackDepth, parameters, priority, handle, core);
}

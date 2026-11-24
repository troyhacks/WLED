// arduino_gpio.cpp - the Arduino GPIO calls, over ESP-IDF's gpio driver.
//
// One deliberate difference from the Arduino core: the core's pinMode() calls
// perimanClearPinBus(), which detaches whatever peripheral owns the pin.  That
// is how the core arbitrates between the LED bus and a UART, but it also means
// a stray pinMode() silently kills a peripheral.  Here the pin is only
// reconfigured, and the peripheral keeps the pin until something else claims
// it.  WLED's pin manager is the authority on which pins belong to what.

#include "Arduino.h"

extern "C" {
#include "driver/gpio.h"
#include "esp_intr_alloc.h"
}

static gpio_isr_t s_isrHandler = nullptr;
static void *s_isrArg = nullptr;
static gpio_num_t s_isrPin = GPIO_NUM_NC;
static portMUX_TYPE s_criticalMux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR gpioIsr(void *arg)
{
    if (s_isrHandler) {
        s_isrHandler(s_isrArg);
    }
}

void pinMode(uint8_t pin, uint8_t mode)
{
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.intr_type = GPIO_INTR_DISABLE;

    switch (mode) {
    case OUTPUT:
        cfg.mode = GPIO_MODE_OUTPUT;
        break;
    case INPUT:
        cfg.mode = GPIO_MODE_INPUT;
        break;
    case INPUT_PULLUP:
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        break;
    case INPUT_PULLDOWN:
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        break;
    case OUTPUT_OPEN_DRAIN:
        cfg.mode = GPIO_MODE_OUTPUT_OD;
        break;
    case INPUT_OUTPUT:
        cfg.mode = GPIO_MODE_INPUT_OUTPUT;
        break;
    default:
        cfg.mode = GPIO_MODE_INPUT;
        break;
    }
    gpio_config(&cfg);
}

void digitalWrite(uint8_t pin, uint8_t val)
{
    gpio_set_level(static_cast<gpio_num_t>(pin), val ? 1 : 0);
}

int digitalRead(uint8_t pin)
{
    return gpio_get_level(static_cast<gpio_num_t>(pin));
}

void attachInterrupt(uint8_t pin, arduino_isr_handler_t handler, int mode)
{
    gpio_num_t gpio = static_cast<gpio_num_t>(pin);

    if (s_isrPin != GPIO_NUM_NC) {
        gpio_isr_handler_remove(s_isrPin);
        gpio_uninstall_isr_service();
        s_isrPin = GPIO_NUM_NC;
    }

    gpio_int_type_t intr = GPIO_INTR_POSEDGE;
    if (mode == FALLING) {
        intr = GPIO_INTR_NEGEDGE;
    } else if (mode == CHANGE) {
        intr = GPIO_INTR_ANYEDGE;
    }

    gpio_set_intr_type(gpio, intr);
    gpio_isr_handler_add(gpio, gpioIsr, nullptr);
    s_isrHandler = handler;
    s_isrArg = nullptr;
    s_isrPin = gpio;
}

void detachInterrupt(uint8_t pin)
{
    gpio_num_t gpio = static_cast<gpio_num_t>(pin);
    if (s_isrPin == gpio) {
        gpio_isr_handler_remove(gpio);
        gpio_uninstall_isr_service();
        s_isrHandler = nullptr;
        s_isrPin = GPIO_NUM_NC;
    }
}

void interrupts()
{
    // The Arduino core means "leave the critical section" here.
    portEXIT_CRITICAL(&s_criticalMux);
}

void noInterrupts()
{
    portENTER_CRITICAL(&s_criticalMux);
}

// arduino_pwm.cpp - analogWrite(), over ESP-IDF's LEDC driver.
//
// The Arduino core implements analogWrite() with the RMT peripheral; LEDC is
// the closer equivalent for a plain duty-cycle output and is what the P4's
// other timers already use.  One channel per pin is claimed on first use.

#include "Arduino.h"

extern "C" {
#include "driver/ledc.h"
#include "soc/soc_caps.h"
}

#if SOC_LEDC_SUPPORTED

namespace {

constexpr int MAX_PWM_PINS = 16;

struct PwmPin {
    int8_t pin = -1;
    ledc_channel_t channel = LEDC_CHANNEL_0;
    ledc_timer_t timer = LEDC_TIMER_0;
    uint32_t freq = 5000;
    uint8_t resolution = 8;
    bool used = false;
};

PwmPin s_pins[MAX_PWM_PINS];
uint32_t s_nextChannel = 0;

PwmPin *findPin(uint8_t pin)
{
    for (auto &p : s_pins) {
        if (p.used && p.pin == pin) {
            return &p;
        }
    }
    return nullptr;
}

PwmPin *claimPin(uint8_t pin, uint32_t freq, uint8_t resolution)
{
    PwmPin *p = findPin(pin);
    if (p) {
        return p;
    }
    for (auto &slot : s_pins) {
        if (!slot.used) {
            slot.used = true;
            slot.pin = pin;
            slot.channel = static_cast<ledc_channel_t>(s_nextChannel % (SOC_LEDC_MAX_CHANNEL));
            slot.timer = static_cast<ledc_timer_t>((s_nextChannel / (SOC_LEDC_MAX_CHANNEL)) % SOC_LEDC_TIMER_MAX);
            slot.freq = freq;
            slot.resolution = resolution;
            s_nextChannel++;
            return &slot;
        }
    }
    return nullptr;
}

} // namespace

// The Arduino core's LEDC calls.  ledcWrite() takes a pin there, and so it
// does here.
bool ledcAttachChannel(uint8_t pin, uint32_t freq, uint8_t resolution, uint8_t channel)
{
    (void)channel;
    PwmPin *p = claimPin(pin, freq, resolution);
    if (!p) {
        return false;
    }
    p->freq = freq;
    p->resolution = resolution;

    ledc_timer_config_t timerCfg = {};
    timerCfg.speed_mode = LEDC_LOW_SPEED_MODE;
    timerCfg.duty_resolution = LEDC_TIMER_8_BIT;
    timerCfg.timer_num = p->timer;
    timerCfg.freq_hz = p->freq;
    timerCfg.clk_cfg = LEDC_AUTO_CLK;

    ledc_channel_config_t channelCfg = {};
    channelCfg.gpio_num = pin;
    channelCfg.speed_mode = LEDC_LOW_SPEED_MODE;
    channelCfg.channel = p->channel;
    channelCfg.timer_sel = p->timer;
    channelCfg.duty = 0;
    channelCfg.hpoint = 0;

    if (ledc_timer_config(&timerCfg) != ESP_OK) {
        return false;
    }
    return ledc_channel_config(&channelCfg) == ESP_OK;
}

bool ledcWrite(uint8_t pin, uint32_t duty)
{
    PwmPin *p = findPin(pin);
    if (!p) {
        p = claimPin(pin, 5000, 8);
        if (!p) {
            return false;
        }
    }
    uint32_t maxDuty = (1UL << p->resolution) - 1;
    if (duty > maxDuty) {
        duty = maxDuty;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, p->channel, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, p->channel);
    return true;
}

bool ledcWriteChannel(uint8_t channel, uint32_t duty)
{
    for (auto &slot : s_pins) {
        if (slot.used && slot.channel == channel) {
            return ledcWrite(slot.pin, duty);
        }
    }
    return false;
}

void analogWriteResolution(uint8_t pin, uint8_t bits)
{
    PwmPin *p = findPin(pin);
    if (p) {
        p->resolution = bits;
    }
}

void analogWriteFrequency(uint8_t pin, uint32_t freq)
{
    PwmPin *p = findPin(pin);
    if (p) {
        p->freq = freq;
    }
}

void analogWrite(uint8_t pin, int value)
{
    if (value < 0) value = 0;
    PwmPin *p = claimPin(pin, 5000, 8);
    if (!p) {
        return;
    }

    ledc_timer_config_t timerCfg = {};
    timerCfg.speed_mode = LEDC_LOW_SPEED_MODE;
    timerCfg.duty_resolution = LEDC_TIMER_8_BIT;
    timerCfg.timer_num = p->timer;
    timerCfg.freq_hz = p->freq;
    timerCfg.clk_cfg = LEDC_AUTO_CLK;

    ledc_channel_config_t channelCfg = {};
    channelCfg.gpio_num = pin;
    channelCfg.speed_mode = LEDC_LOW_SPEED_MODE;
    channelCfg.channel = p->channel;
    channelCfg.timer_sel = p->timer;
    channelCfg.duty = 0;
    channelCfg.hpoint = 0;

    if (ledc_timer_config(&timerCfg) != ESP_OK) {
        return;
    }
    if (ledc_channel_config(&channelCfg) != ESP_OK) {
        return;
    }

    uint32_t duty = (uint32_t)value;
    uint32_t maxDuty = (1UL << p->resolution) - 1;
    if (duty > maxDuty) {
        duty = maxDuty;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, p->channel, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, p->channel);
}

#else

void analogWriteResolution(uint8_t, uint8_t) {}
void analogWriteFrequency(uint8_t, uint32_t) {}
void analogWrite(uint8_t, int) {}

#endif // SOC_LEDC_SUPPORTED

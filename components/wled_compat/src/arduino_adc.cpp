// arduino_adc.cpp - analogRead(), over ESP-IDF's ADC oneshot driver.
//
// The Arduino core reads the ADC with the same driver and adds the calibration
// curve on top, so the numbers match what the Arduino build produced.

#include "Arduino.h"

extern "C" {
#include "driver/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "soc/soc_caps.h"
}

#if SOC_ADC_PERIPH_NUM > 0

namespace {

adc_oneshot_unit_handle_t s_unit1 = nullptr;
adc_oneshot_unit_handle_t s_unit2 = nullptr;
bool s_caliReady = false;
adc_cali_handle_t s_cali = nullptr;

// ADC1 has eight channels, ADC2 six, and the Arduino core numbers them
// across the two units.
constexpr int8_t ADC1_CHANNELS = 8;

bool ensureUnit(int8_t channel, adc_oneshot_unit_handle_t *unit)
{
    if (channel < ADC1_CHANNELS) {
        if (!s_unit1) {
            adc_oneshot_unit_init_cfg_t unitCfg = {};
            unitCfg.unit_id = ADC_UNIT_1;
            if (adc_oneshot_new_unit(&unitCfg, &s_unit1) != ESP_OK) {
                s_unit1 = nullptr;
                return false;
            }
        }
        *unit = s_unit1;
        return true;
    }
    if (!s_unit2) {
        adc_oneshot_unit_init_cfg_t unitCfg = {};
        unitCfg.unit_id = ADC_UNIT_2;
        if (adc_oneshot_new_unit(&unitCfg, &s_unit2) != ESP_OK) {
            s_unit2 = nullptr;
            return false;
        }
    }
    *unit = s_unit2;
    return true;
}

bool ensureCalibration()
{
    if (s_caliReady) {
        return true;
    }
    adc_cali_curve_fitting_config_t caliCfg = {};
    caliCfg.unit_id = ADC_UNIT_1;
    caliCfg.chan = ADC_CHANNEL_0;
    caliCfg.atten = ADC_ATTEN_DB_12;
    caliCfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&caliCfg, &s_cali) == ESP_OK) {
        s_caliReady = true;
    }
    return s_caliReady;
}

} // namespace

int analogRead(uint8_t pin)
{
    int8_t channel = digitalPinToAnalogChannel(pin);
    if (channel < 0) {
        return 0;
    }
    adc_oneshot_unit_handle_t unit = nullptr;
    if (!ensureUnit(channel, &unit)) {
        return 0;
    }

    adc_channel_t adcChannel = (channel < ADC1_CHANNELS)
                                   ? static_cast<adc_channel_t>(channel)
                                   : static_cast<adc_channel_t>(channel - ADC1_CHANNELS);

    adc_oneshot_chan_cfg_t chanCfg = {};
    chanCfg.atten = ADC_ATTEN_DB_12;
    chanCfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(unit, adcChannel, &chanCfg) != ESP_OK) {
        return 0;
    }

    int raw = 0;
    if (adc_oneshot_read(unit, adcChannel, &raw) != ESP_OK) {
        return 0;
    }
    if (!ensureCalibration()) {
        return raw;
    }
    int value = 0;
    if (adc_cali_raw_to_voltage(s_cali, raw, &value) != ESP_OK) {
        return raw;
    }
    // The Arduino core reports the 12 bit reading scaled to 0..4095.
    return (value * 4095) / 3300;
}

void analogReadResolution(uint8_t bits)
{
    // The Arduino core's resolution knob does not change the reading's scale
    // on the ESP32; the value is always 12 bit.
    (void)bits;
}

#else

int analogRead(uint8_t) { return 0; }
void analogReadResolution(uint8_t) {}

#endif

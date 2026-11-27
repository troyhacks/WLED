// arduino_touch.cpp - touchRead(), the Arduino core's touch sensor read.
//
// The core has it for the chips that have a touch sensor.  Under the IDF
// the driver is the IDF's own (esp_driver_touch_sens), and this is the
// same sequence the IDF's own example runs, with the same defaults.
//
// The value is the smooth count: the pad's capacitance goes up when it is
// touched, and the count the peripheral reports goes DOWN, so a *smaller*
// number means "touched".  WLED's button code compares the reading against
// a threshold that way.
//
// The pads are set up as they are read, one at a time: the peripheral is
// brought up on the first read, and a pad is configured when it is read
// for the first time.
//
// The touch hardware version 1 and 2 - the classic ESP32, the S2 and the S3.
// The version 3 (the P4) is not here: WLED's button code only asks for
// touchRead on the chips that are not RISC-V, and the P4's touch panel is
// a separate controller on the I2C bus anyway.

#include "Arduino.h"

#if SOC_TOUCH_SENSOR_SUPPORTED && SOC_TOUCH_SENSOR_VERSION <= 2

extern "C" {
#include "driver/touch_sens.h"
}

namespace {

touch_sensor_handle_t s_touch = nullptr;                    // the peripheral
touch_channel_handle_t s_touch_chan[SOC_TOUCH_MAX_CHAN_ID + 1] = {}; // a pad
touch_channel_config_t s_touch_pad_cfg = {};                 // the pad's setup

void touchBringUp()
{
    if (s_touch) {
        return;
    }
#if SOC_TOUCH_SENSOR_VERSION == 1
    touch_sensor_sample_config_t sample_cfg[TOUCH_SAMPLE_CFG_NUM] = {
        TOUCH_SENSOR_V1_DEFAULT_SAMPLE_CONFIG(5.0, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_1V7)
    };
    touch_channel_config_t chan_cfg = {
        .abs_active_thresh = {1000},
        .charge_speed = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
        .group = TOUCH_CHAN_TRIG_GROUP_BOTH,
    };
#else
    touch_sensor_sample_config_t sample_cfg[TOUCH_SAMPLE_CFG_NUM] = {
        TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(500, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_2V2)
    };
    touch_channel_config_t chan_cfg = {
        .active_thresh = {2000},
        .charge_speed = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
    };
#endif
    touch_sensor_config_t sens_cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(TOUCH_SAMPLE_CFG_NUM, sample_cfg);
    if (touch_sensor_new_controller(&sens_cfg, &s_touch) != ESP_OK) {
        s_touch = nullptr;
        return;
    }
    touch_sensor_filter_config_t filter_cfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    touch_sensor_config_filter(s_touch, &filter_cfg);
    touch_sensor_enable(s_touch);
    // The pad configurations are kept here, so that the channel setup
    // below can use them.
    s_touch_pad_cfg = chan_cfg;
}

} // namespace

uint32_t touchRead(uint8_t pin)
{
    int8_t pad = digitalPinToTouchChannel(pin);
    if (pad < 0 || pad > SOC_TOUCH_MAX_CHAN_ID) {
        // No touch pad on this pin.  The count the peripheral reports is
        // never that high, so this reads as "not touched" - the right way
        // round.
        return UINT32_MAX;
    }
    touchBringUp();
    if (!s_touch) {
        return UINT32_MAX;
    }
    if (!s_touch_chan[pad]) {
        if (touch_sensor_new_channel(s_touch, pad, &s_touch_pad_cfg, &s_touch_chan[pad]) != ESP_OK) {
            return UINT32_MAX;
        }
    }
    uint32_t value = 0;
    if (touch_channel_read_data(s_touch_chan[pad], TOUCH_CHAN_DATA_TYPE_SMOOTH, &value) != ESP_OK) {
        return UINT32_MAX;
    }
    return value;
}

#endif // SOC_TOUCH_SENSOR_SUPPORTED && SOC_TOUCH_SENSOR_VERSION <= 2

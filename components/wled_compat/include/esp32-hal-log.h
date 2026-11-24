// esp32-hal-log.h - the Arduino core's logging header.
//
// AsyncTCP includes this one.  The core's version prints through the core's
// own log_printf(); here the same messages go through ESP-IDF's logger.

#pragma once

#include "sdkconfig.h"
#include "esp_log.h"

#define ARDUHAL_LOG_LEVEL_NONE (0)
#define ARDUHAL_LOG_LEVEL_ERROR (1)
#define ARDUHAL_LOG_LEVEL_WARN (2)
#define ARDUHAL_LOG_LEVEL_INFO (3)
#define ARDUHAL_LOG_LEVEL_DEBUG (4)
#define ARDUHAL_LOG_LEVEL_VERBOSE (5)

#ifndef CONFIG_ARDUHAL_LOG_DEFAULT_LEVEL
#define CONFIG_ARDUHAL_LOG_DEFAULT_LEVEL ARDUHAL_LOG_LEVEL_NONE
#endif

#ifdef CORE_DEBUG_LEVEL
#define ARDUHAL_LOG_LEVEL CORE_DEBUG_LEVEL
#else
#define ARDUHAL_LOG_LEVEL CONFIG_ARDUHAL_LOG_DEFAULT_LEVEL
#endif

#ifndef log_printf
#define log_printf(format, ...) ESP_LOGI("arduino", format, ##__VA_ARGS__)
#endif

#ifndef log_e
#define log_e(format, ...) ESP_LOGE("arduino", format, ##__VA_ARGS__)
#endif
#ifndef log_w
#define log_w(format, ...) ESP_LOGW("arduino", format, ##__VA_ARGS__)
#endif
#ifndef log_i
#define log_i(format, ...) ESP_LOGI("arduino", format, ##__VA_ARGS__)
#endif
#ifndef log_d
#define log_d(format, ...) ESP_LOGD("arduino", format, ##__VA_ARGS__)
#endif
#ifndef log_v
#define log_v(format, ...) ESP_LOGV("arduino", format, ##__VA_ARGS__)
#endif

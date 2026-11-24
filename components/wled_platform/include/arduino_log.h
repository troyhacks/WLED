// arduino_log.h - the Arduino core's logging macros, on ESP-IDF's logger.
//
// The Arduino core logs with log_e() and log_w(); ESP-IDF logs with
// ESP_LOGE() and ESP_LOGW().  The vendored Arduino files use the core's
// spelling, so this maps it.

#pragma once

#include "esp_log.h"

#ifndef log_e
#define log_e(...) ESP_LOGE("fs", __VA_ARGS__)
#endif
#ifndef log_w
#define log_w(...) ESP_LOGW("fs", __VA_ARGS__)
#endif
#ifndef log_i
#define log_i(...) ESP_LOGI("fs", __VA_ARGS__)
#endif
#ifndef log_d
#define log_d(...) ESP_LOGD("fs", __VA_ARGS__)
#endif
#ifndef log_v
#define log_v(...) ESP_LOGV("fs", __VA_ARGS__)
#endif

// The core's log helpers: the file name out of a path, and the watchdog
// switches for the second core.
const char *pathToFileName(const char *path);
void enableCore0WDT();
bool disableCore0WDT();
void enableCore1WDT();
bool disableCore1WDT();

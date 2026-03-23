/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
  #endif

  /**
   * @brief Perform LittleFS OTA update
   *
   * @param delete_after_use Whether to delete firmware file after successful flash
   * @return esp_err_t ESP_OK on success
   */
  esp_err_t ota_littlefs_perform(bool delete_after_use);

  /**
   * @brief Flash C6 WiFi coprocessor firmware directly from any POSIX path (SD card, USB, etc.)
   *        Reads via fopen() — no copy to LittleFS needed.
   *        Calls esp_hosted_slave_ota_activate() on success; caller must reboot.
   *
   * @param firmware_path  Full POSIX path e.g. "/sdcard/wled_update/network_adapter.bin"
   * @return ESP_OK              — flashed and activated, please esp_restart()
   *         ESP_ERR_NOT_FOUND   — version matches, no flash needed
   *         other               — failure, file untouched
   */
  esp_err_t ota_from_path(const char* firmware_path);

  #ifdef __cplusplus
}
#endif
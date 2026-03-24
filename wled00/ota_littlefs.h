/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

esp_err_t ota_littlefs_perform(bool delete_after_use);
esp_err_t ota_from_path(const char* firmware_path);
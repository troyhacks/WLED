/*
 * esp_lcd_ili9488.h
 *
 * A panel driver for the ILI9488, on top of ESP-IDF's esp_lcd.
 *
 * PROVENANCE
 * ----------
 * The panel sequence and the structure of this driver come from Espressif's
 * own esp_lcd_ili9488 component, as it is shipped in WaveShare's Arduino
 * demo for the ESP32-C5-LCD-2.73 board (which is where this board's wiring
 * comes from too). The driver is Apache-2.0, like the rest of ESP-IDF.
 *
 * WHAT IS DIFFERENT HERE
 * ----------------------
 *  1. It does not include "lv_conf.h". The LVGL dependency was only there
 *     for LV_COLOR_16_SWAP, and this driver does not care about LVGL.
 *  2. The colour mode is chosen at run time instead of being hard-wired to
 *     18 bit. The original sent COLMOD=0x66 in its init table whatever the
 *     panel was configured for, so a 16 bit panel would have been fed 18 bit
 *     data. Here the COLMOD command follows bits_per_pixel.
 *  3. It only compiles when WLEDMM_C5_DISPLAY is defined, so the other
 *     boards in this repo do not carry it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create an ILI9488 panel on top of an esp_lcd IO handle.
 *
 * @param io                The IO handle (SPI) the panel is wired to.
 * @param panel_dev_config  Panel configuration. bits_per_pixel may be 16
 *                           (RGB565, the fast path - the data is sent as it
 *                           is) or anything else (the driver then converts
 *                           RGB565 to RGB666 on the way out).
 * @param buffer_pixels     Size of the conversion buffer, in pixels. Only
 *                           needed for the converting path; may be 0 for 16 bit.
 * @param ret_panel         Out: the panel handle.
 */
esp_err_t esp_lcd_new_panel_ili9488(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    const size_t buffer_pixels,
                                    esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif

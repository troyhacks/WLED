/*
 * esp_lcd_ili9488.c - see the header for the provenance and the changes.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_lcd_ili9488.h"

#if defined(WLEDMM_C5_DISPLAY)   // only the ESP32-C5 remote carries this

#include "esp_idf_version.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ili9488";

typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t data_bytes;
} lcd_init_cmd_t;

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t memory_access_control;
    uint8_t color_mode;
    size_t buffer_pixels;
    uint8_t *color_buffer;
} ili9488_panel_t;

enum {
    ILI9488_COLOR_MODE_16BIT = 0x55,
    ILI9488_COLOR_MODE_18BIT = 0x66,
    ILI9488_INIT_LENGTH_MASK = 0x1F,
    ILI9488_INIT_DONE_FLAG = 0xFF,
};

static esp_err_t panel_ili9488_del(esp_lcd_panel_t *panel)
{
    ili9488_panel_t *ili9488 = __containerof(panel, ili9488_panel_t, base);
    if (ili9488->reset_gpio_num >= 0) {
        gpio_reset_pin(ili9488->reset_gpio_num);
    }
    if (ili9488->color_buffer) {
        heap_caps_free(ili9488->color_buffer);
    }
    free(ili9488);
    return ESP_OK;
}

static esp_err_t panel_ili9488_reset(esp_lcd_panel_t *panel)
{
    ili9488_panel_t *ili9488 = __containerof(panel, ili9488_panel_t, base);
    if (ili9488->reset_gpio_num >= 0) {
        gpio_set_level(ili9488->reset_gpio_num, ili9488->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(ili9488->reset_gpio_num, !ili9488->reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        esp_lcd_panel_io_tx_param(ili9488->io, LCD_CMD_SWRESET, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    return ESP_OK;
}

static esp_err_t panel_ili9488_init(esp_lcd_panel_t *panel)
{
    ili9488_panel_t *ili9488 = __containerof(panel, ili9488_panel_t, base);
    esp_lcd_panel_io_handle_t io = ili9488->io;

    const lcd_init_cmd_t init_cmds[] = {
        {0xC0, {0x10, 0x12}, 2},
        {0xC1, {0x66}, 1},
        {0xC5, {0x00, 0x45, 0x80}, 3},
        {LCD_CMD_MADCTL, {0x48}, 1},
        {LCD_CMD_INVON, {0x00}, 0},
        {0xB0, {0x80}, 1},
        {0xB1, {0xA0}, 1},
        {0xB4, {0x02}, 1},
        {0xB6, {0x00, 0x02, 0x3B}, 3},
        {0xE0, {0x00, 0x08, 0x19, 0x05, 0x16, 0x09, 0x35, 0x47, 0x4A, 0x04, 0x0D, 0x0C, 0x30, 0x33, 0x0F}, 15},
        {0xE1, {0x00, 0x08, 0x19, 0x05, 0x16, 0x09, 0x35, 0x47, 0x4A, 0x04, 0x0D, 0x0C, 0x30, 0x33, 0x0F}, 15},
        {0xE9, {0x00}, 1},
        {0xF7, {0xA9, 0x51, 0x2C, 0x82}, 4},
        {0x00, {0x00}, ILI9488_INIT_DONE_FLAG},
    };

    for (int i = 0; init_cmds[i].data_bytes != ILI9488_INIT_DONE_FLAG; i++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, init_cmds[i].cmd,
                            init_cmds[i].data,
                            init_cmds[i].data_bytes & ILI9488_INIT_LENGTH_MASK),
                            TAG, "send init command failed");
    }

    // THE COLOUR MODE, last, and it follows what the panel was asked for.
    //
    // The original driver sent COLMOD=0x66 (18 bit) in its table whatever
    // bits_per_pixel said, so a 16 bit panel would have been handed 18 bit
    // data. Here the panel is told what it is going to be fed.
    {
        const uint8_t colmod = (ili9488->color_mode == ILI9488_COLOR_MODE_16BIT)
                             ? ILI9488_COLOR_MODE_16BIT : ILI9488_COLOR_MODE_18BIT;
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, &colmod, 1),
                            TAG, "set colour mode failed");
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0), TAG, "sleep out failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_DISPON, NULL, 0), TAG, "display on failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

#define SEND_COORDS(start, end, io, cmd) \
    esp_lcd_panel_io_tx_param(io, cmd, (uint8_t[]) { \
        (uint8_t)(((start) >> 8) & 0xFF), \
        (uint8_t)((start) & 0xFF), \
        (uint8_t)((((end) - 1) >> 8) & 0xFF), \
        (uint8_t)(((end) - 1) & 0xFF), \
    }, 4)

static esp_err_t panel_ili9488_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    ili9488_panel_t *ili9488 = __containerof(panel, ili9488_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end));

    x_start += ili9488->x_gap;
    x_end += ili9488->x_gap;
    y_start += ili9488->y_gap;
    y_end += ili9488->y_gap;

    const size_t pixel_count = (size_t)(x_end - x_start) * (y_end - y_start);
    ESP_RETURN_ON_ERROR(SEND_COORDS(x_start, x_end, ili9488->io, LCD_CMD_CASET), TAG, "set column failed");
    ESP_RETURN_ON_ERROR(SEND_COORDS(y_start, y_end, ili9488->io, LCD_CMD_RASET), TAG, "set row failed");

    if (ili9488->color_mode == ILI9488_COLOR_MODE_18BIT) {
        ESP_RETURN_ON_FALSE(pixel_count <= ili9488->buffer_pixels, ESP_ERR_INVALID_SIZE, TAG,
                            "draw area too large: %zu > %zu", pixel_count, ili9488->buffer_pixels);
        uint8_t *buf = ili9488->color_buffer;
        const uint16_t *src = (const uint16_t *)color_data;
        for (size_t i = 0, j = 0; i < pixel_count; i++) {
            uint16_t rgb565 = src[i];
            buf[j++] = (uint8_t)(((rgb565 & 0xF800) >> 8) | ((rgb565 & 0x8000) >> 13));
            buf[j++] = (uint8_t)((rgb565 & 0x07E0) >> 3);
            buf[j++] = (uint8_t)(((rgb565 & 0x001F) << 3) | ((rgb565 & 0x0010) >> 2));
        }
        return esp_lcd_panel_io_tx_color(ili9488->io, LCD_CMD_RAMWR, buf, pixel_count * 3);
    }

    return esp_lcd_panel_io_tx_color(ili9488->io, LCD_CMD_RAMWR, color_data, pixel_count * 2);
}

#undef SEND_COORDS

static esp_err_t panel_ili9488_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    ili9488_panel_t *ili9488 = __containerof(panel, ili9488_panel_t, base);
    return esp_lcd_panel_io_tx_param(ili9488->io, invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF, NULL, 0);
}

static esp_err_t panel_ili9488_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    ili9488_panel_t *ili9488 = __containerof(panel, ili9488_panel_t, base);
    if (mirror_x) {
        ili9488->memory_access_control &= ~LCD_CMD_MX_BIT;
    } else {
        ili9488->memory_access_control |= LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        ili9488->memory_access_control |= LCD_CMD_MY_BIT;
    } else {
        ili9488->memory_access_control &= ~LCD_CMD_MY_BIT;
    }
    return esp_lcd_panel_io_tx_param(ili9488->io, LCD_CMD_MADCTL, &ili9488->memory_access_control, 1);
}

static esp_err_t panel_ili9488_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    ili9488_panel_t *ili9488 = __containerof(panel, ili9488_panel_t, base);
    if (swap_axes) {
        ili9488->memory_access_control |= LCD_CMD_MV_BIT;
    } else {
        ili9488->memory_access_control &= ~LCD_CMD_MV_BIT;
    }
    return esp_lcd_panel_io_tx_param(ili9488->io, LCD_CMD_MADCTL, &ili9488->memory_access_control, 1);
}

static esp_err_t panel_ili9488_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    ili9488_panel_t *ili9488 = __containerof(panel, ili9488_panel_t, base);
    ili9488->x_gap = x_gap;
    ili9488->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_ili9488_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    ili9488_panel_t *ili9488 = __containerof(panel, ili9488_panel_t, base);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(ili9488->io, on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF, NULL, 0), TAG, "disp on/off failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_ili9488(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    const size_t buffer_pixels,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    ili9488_panel_t *ili9488 = (ili9488_panel_t *)calloc(1, sizeof(ili9488_panel_t));
    ESP_RETURN_ON_FALSE(ili9488, ESP_ERR_NO_MEM, TAG, "no mem for panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        esp_rom_gpio_pad_select_gpio(panel_dev_config->reset_gpio_num);
        gpio_config_t cfg = {
            .pin_bit_mask = BIT64(panel_dev_config->reset_gpio_num),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&cfg), err, TAG, "reset gpio config failed");
    }

    if (panel_dev_config->bits_per_pixel == 16) {
        ili9488->color_mode = ILI9488_COLOR_MODE_16BIT;
    } else {
        ESP_GOTO_ON_FALSE(buffer_pixels > 0, ESP_ERR_INVALID_ARG, err, TAG, "buffer size required");
        ili9488->color_mode = ILI9488_COLOR_MODE_18BIT;
        ili9488->buffer_pixels = buffer_pixels;
        ili9488->color_buffer = (uint8_t *)heap_caps_malloc(buffer_pixels * 3, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (!ili9488->color_buffer) {
            ili9488->color_buffer = (uint8_t *)heap_caps_malloc(buffer_pixels * 3, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        ESP_GOTO_ON_FALSE(ili9488->color_buffer, ESP_ERR_NO_MEM, err, TAG, "no mem for color buffer");
    }

    ili9488->memory_access_control = LCD_CMD_MX_BIT | LCD_CMD_BGR_BIT;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    if (panel_dev_config->rgb_ele_order == LCD_RGB_ELEMENT_ORDER_RGB) {
        ili9488->memory_access_control &= ~LCD_CMD_BGR_BIT;
    }
#else
    if (panel_dev_config->color_space == ESP_LCD_COLOR_SPACE_RGB) {
        ili9488->memory_access_control &= ~LCD_CMD_BGR_BIT;
    }
#endif

    ili9488->io = io;
    ili9488->reset_gpio_num = panel_dev_config->reset_gpio_num;
    ili9488->reset_level = panel_dev_config->flags.reset_active_high;
    ili9488->base.del = panel_ili9488_del;
    ili9488->base.reset = panel_ili9488_reset;
    ili9488->base.init = panel_ili9488_init;
    ili9488->base.draw_bitmap = panel_ili9488_draw_bitmap;
    ili9488->base.invert_color = panel_ili9488_invert_color;
    ili9488->base.set_gap = panel_ili9488_set_gap;
    ili9488->base.mirror = panel_ili9488_mirror;
    ili9488->base.swap_xy = panel_ili9488_swap_xy;
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    ili9488->base.disp_off = panel_ili9488_disp_on_off;
#else
    ili9488->base.disp_on_off = panel_ili9488_disp_on_off;
#endif

    *ret_panel = &ili9488->base;
    return ESP_OK;

err:
    if (ili9488) {
        if (ili9488->color_buffer) {
            heap_caps_free(ili9488->color_buffer);
        }
        free(ili9488);
    }
    return ret;
}


#endif  // WLEDMM_C5_DISPLAY

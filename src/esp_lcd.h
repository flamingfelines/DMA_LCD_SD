/*
 * Copyright (c) 2023 Russ Hughes
 * Copyright (c) 2026 FlamingFelines
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE OF ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef __ESP_LCD_H__
#define __ESP_LCD_H__

#include "mphalport.h"
#include "py/obj.h"
#include "esp_lcd_panel_io.h"
#include "driver/spi_master.h"
#include "esp_spi.h"
#include <stdbool.h>

extern volatile bool lcd_panel_active;
bool lcd_panel_done(esp_lcd_panel_io_handle_t panel_io,
                    esp_lcd_panel_io_event_data_t *edata,
                    void *user_ctx);

// ── SPI Bus object ────────────────────────────────────────────────────────────

typedef struct _esp_lcd_spi_bus_obj_t {
    mp_obj_base_t base;
    char *name;
    mp_obj_t bus_obj;
    int spi_host;
    int dc_gpio_num;
    int cs_gpio_num;
    int spi_mode;
    unsigned int pclk_hz;
    int lcd_cmd_bits;
    int lcd_param_bits;
    esp_lcd_panel_io_handle_t io_handle;
    spi_device_handle_t spi_dev;

    struct {
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
        unsigned int dc_as_cmd_phase:1;
#endif
        unsigned int dc_low_on_data:1;
        unsigned int octal_mode:1;
        unsigned int lsb_first:1;
        unsigned int swap_color_bytes:1;
    } flags;
} esp_lcd_spi_bus_obj_t;

extern const mp_obj_type_t esp_lcd_spi_bus_type;

// ── Display object ────────────────────────────────────────────────────────────

typedef struct _anim_display_obj_t {
    mp_obj_base_t              base;
    mp_obj_t                   bus;
    esp_lcd_panel_io_handle_t  io_handle;
    esp_lcd_panel_handle_t     panel_handle;
    uint16_t                   width;
    uint16_t                   height;
    uint8_t                    rotation;
    bool                       inversion_mode;
    bool                       swap_color_bytes;
    gpio_num_t                 rst;
    uint8_t                    color_space;
    uint16_t                   dma_rows;
    uint16_t                  *dma_buffer;
    size_t                     dma_buffer_size;
} anim_display_obj_t;

extern const mp_obj_type_t anim_display_type;

#endif /* __ESP_LCD_H__ */

/*
 * Copyright (c) 2023 Russ Hughes
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef __s3lcd_spi_bus_H__
#define __s3lcd_spi_bus_H__
#include "mphalport.h"
#include "py/obj.h"
#include "esp_lcd_panel_io.h"
#include "driver/spi_master.h"
#include "esp_spi.h"

bool lcd_panel_done(esp_lcd_panel_io_handle_t panel_io,
                    esp_lcd_panel_io_event_data_t *edata,
                    void *user_ctx);

// LCD SPI Bus Configuration Structure
// Add this to your existing s3lcd_spi_bus.h

typedef struct _s3lcd_spi_bus_obj_t {
    mp_obj_base_t base;
    char *name;
    mp_obj_t bus_obj;                           // Reference to esp_spi.SPIBus object
    int spi_host;                               // SPI host number
    int dc_gpio_num;                            // DC/RS GPIO pin
    int cs_gpio_num;                            // CS GPIO pin
    int spi_mode;                               // SPI mode (0-3)
    unsigned int pclk_hz;                       // Pixel clock frequency
    int lcd_cmd_bits;                           // LCD command bits
    int lcd_param_bits;                         // LCD parameter bits
    esp_lcd_panel_io_handle_t io_handle;        // LCD panel IO handle - ADD THIS
    spi_device_handle_t spi_dev;                // SPI device handle (can keep for compatibility)
    
    struct {
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
        unsigned int dc_as_cmd_phase:1;
#endif
        unsigned int dc_low_on_data:1;
        unsigned int octal_mode:1;
        unsigned int lsb_first:1;
        unsigned int swap_color_bytes:1;
    } flags;
} s3lcd_spi_bus_obj_t;

extern const mp_obj_type_t s3lcd_spi_bus_type;

#endif /* __s3lcd_spi_bus_H__ */

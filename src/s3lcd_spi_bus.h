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
#include "extmod/modmachine.h" // for mp_machine_spi_p_t

// SPI Bus Configuration Structure
typedef struct _s3lcd_spi_bus_obj_t {
    mp_obj_base_t base;                     // base class
    char *name;                             // name of the display
    mp_obj_t spi_obj;                       // The Python machine.SPI object we attach to
    spi_device_handle_t spi_dev;            // The handle for this display device on the SPI bus
    int spi_host;                           // SPI host (SPI2_HOST, SPI3_HOST, etc.)
    
    // GPIO configuration
    int cs_gpio_num;                        // GPIO used for CS line
    int dc_gpio_num;                        // GPIO used to select the D/C line, set this to -1 if the D/C line not controlled by manually pulling high/low GPIO
    
    // SPI configuration parameters
    int spi_mode;                           // Traditional SPI mode (0~3)
    unsigned int pclk_hz;                   // Frequency of pixel clock
    int lcd_cmd_bits;                       // Bit-width of LCD command
    int lcd_param_bits;                     // Bit-width of LCD parameter
    
    // Flags for SPI behavior
    struct {
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
        unsigned int dc_as_cmd_phase: 1;    // D/C line value is encoded into SPI transaction command phase
#endif
        unsigned int dc_low_on_data: 1;     // If this flag is enabled, DC line = 0 means transfer data, DC line = 1 means transfer command; vice versa
        unsigned int octal_mode: 1;         // transmit with octal mode (8 data lines), this mode is used to simulate Intel 8080 timing
        unsigned int lsb_first: 1;          // transmit LSB bit first
        unsigned int swap_color_bytes: 1;   // Swap color bytes in 16-bit color mode
    } flags;
} s3lcd_spi_bus_obj_t;

extern const mp_obj_type_t s3lcd_spi_bus_type;

#endif /* __s3lcd_spi_bus_H__ */

/* Based off of Russ Hughes s3lcd software
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

#ifndef ESP_SPI_H
#define ESP_SPI_H

#include "py/obj.h"
#include "driver/spi_master.h"
#include <stdbool.h>

// SPIBus object struct
typedef struct esp_spi_bus_obj_t {
    mp_obj_base_t base;
    int miso_io_num;
    int mosi_io_num;
    int sclk_io_num;
    spi_host_device_t host;  // e.g. SPI2_HOST
    bool initialized;
} esp_spi_bus_obj_t;

extern const mp_obj_type_t esp_spi_bus_type;

// Device object struct  
typedef struct esp_spi_device_obj_t {
    mp_obj_base_t base;
    spi_device_handle_t spi_dev_handle;
} esp_spi_device_obj_t;

extern const mp_obj_type_t esp_spi_device_type;

#endif // ESP_SPI_H

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

#ifndef ESP_SD_H
#define ESP_SD_H

#include "py/obj.h"
#include "driver/sdmmc_types.h"
#include "driver/sdspi_host.h"
#include <stdbool.h>
#include <stdint.h>

// SD Card object struct - updated to match new implementation
typedef struct esp_sd_obj_t {
    mp_obj_base_t base;
    sdmmc_card_t *card;
    mp_obj_t bus;           // SPI bus object
    int cs_pin;             // CS pin number
    bool initialized;       // Changed: was 'mounted', now 'initialized'
    uint32_t block_count;   // Added: total number of blocks
    uint32_t block_size;    // Added: block size in bytes
    sdspi_dev_handle_t spi_handle; // Added: SPI device handle
} esp_sd_obj_t;

// Type declaration
extern const mp_obj_type_t esp_sd_type;

// Module declaration
extern const mp_obj_module_t esp_sd_module;

#endif // ESP_SD_H

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

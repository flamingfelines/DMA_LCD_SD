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

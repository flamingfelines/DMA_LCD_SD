#ifndef ESP_SD_H
#define ESP_SD_H
#include "py/obj.h"
#include "driver/sdmmc_types.h"
// SD Card object struct
typedef struct esp_sd_obj_t {
    mp_obj_base_t base;
    sdmmc_card_t *card;
    char *mount_point;
    mp_obj_t bus;           
    int cs_pin;            
} esp_sd_obj_t;
// Type declaration
extern const mp_obj_type_t esp_sd_type;

// Module declaration
extern const mp_obj_module_t esp_sd_module;
#endif // ESP_SD_H

#ifndef ESP_SD_H
#define ESP_SD_H

#include "py/obj.h"
#include "driver/sdmmc_types.h"

// SD Card object struct
typedef struct esp_sd_obj_t {
    mp_obj_base_t base;
    sdmmc_card_t *card;
    char *mount_point;
    bool mounted;
} esp_sd_obj_t;

// Type declaration
extern const mp_obj_type_t esp_sd_type;

// Function declarations
mp_obj_t esp_sd_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args);
mp_obj_t esp_sd_mount(size_t n_args, const mp_obj_t *args);
mp_obj_t esp_sd_umount(mp_obj_t self_in);
mp_obj_t esp_sd_deinit(mp_obj_t self_in);
void esp_sd_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind);

// Module declaration
extern const mp_obj_module_t esp_sd_module;

#endif // ESP_SD_H

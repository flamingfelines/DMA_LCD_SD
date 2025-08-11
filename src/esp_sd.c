#include "py/obj.h"
#include "py/runtime.h"
#include "py/mperrno.h"

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"

#include "esp_spi.h" // Your esp_spi.SPIBus definition header

static const char *TAG = "esp_sd";

typedef struct _esp_sd_obj_t {
    mp_obj_base_t base;
    sdmmc_card_t *card;
    const char *mount_point;
} esp_sd_obj_t;

STATIC mp_obj_t esp_sd_mount(mp_obj_t bus_in, mp_int_t cs_pin, mp_obj_t mount_point_in) {
    // Validate bus object type
    if (!mp_obj_is_type(bus_in, &esp_spi_type)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Expected esp_spi.SPIBus object"));
    }

    esp_spi_obj_t *bus = MP_OBJ_TO_PTR(bus_in);
    spi_host_device_t host_id = bus->host_id;

    const char *mount_point = mp_obj_str_get_str(mount_point_in);

    // SD card slot configuration
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = cs_pin;
    slot_config.host_id = host_id;

    // Mount configuration
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    // Card handle
    sdmmc_card_t *card;
    esp_err_t ret = esp_vfs_fat_sdspi_mount(mount_point, &host_id, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        mp_raise_OSError(ret);
    }

    ESP_LOGI(TAG, "SD card mounted at %s", mount_point);
    sdmmc_card_print_info(stdout, card);

    // Create Python object
    esp_sd_obj_t *o = m_new_obj(esp_sd_obj_t);
    o->base.type = &esp_sd_type;
    o->card = card;
    o->mount_point = mount_point;
    return MP_OBJ_FROM_PTR(o);
}

STATIC mp_obj_t esp_sd_umount(mp_obj_t self_in) {
    esp_sd_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->card) {
        mp_raise_ValueError(MP_ERROR_TEXT("Not mounted"));
    }
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(self->mount_point, self->card);
    if (ret != ESP_OK) {
        mp_raise_OSError(ret);
    }
    self->card = NULL;
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(esp_sd_umount_obj, esp_sd_umount);

STATIC MP_DEFINE_CONST_FUN_OBJ_3(esp_sd_mount_obj, esp_sd_mount);

STATIC const mp_rom_map_elem_t esp_sd_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_umount), MP_ROM_PTR(&esp_sd_umount_obj) },
};
STATIC MP_DEFINE_CONST_DICT(esp_sd_locals_dict, esp_sd_locals_dict_table);

const mp_obj_type_t esp_sd_type = {
    { &mp_type_type },
    .name = MP_QSTR_SDCard,
    .locals_dict = (mp_obj_dict_t *)&esp_sd_locals_dict,
};

STATIC const mp_rom_map_elem_t esp_sd_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_esp_sd) },
    { MP_ROM_QSTR(MP_QSTR_mount), MP_ROM_PTR(&esp_sd_mount_obj) },
};
STATIC MP_DEFINE_CONST_DICT(esp_sd_module_globals, esp_sd_module_globals_table);

const mp_obj_module_t esp_sd_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&esp_sd_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_esp_sd, esp_sd_module);

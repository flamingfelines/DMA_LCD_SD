#include "py/obj.h"
#include "py/runtime.h"
#include "py/mperrno.h"
#include "py/mpconfig.h"
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_spi.h"

static const char *TAG = "esp_sd";

static const char *TAG = "esp_sd";

typedef struct esp_sd_obj_t {
    mp_obj_base_t base;
    sdmmc_card_t *card;
    char *mount_point;
    mp_obj_t bus;           // Store the SPI bus object
    int cs_pin;             // Store the CS pin
    bool mounted;
} esp_sd_obj_t;

static void esp_sd_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    esp_sd_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<esp_sd.SDCard mounted=%d mount_point=%s cs=%d>", 
              self->mounted, self->mount_point ? self->mount_point : "None", self->cs_pin);
}

static mp_obj_t esp_sd_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_bus, ARG_cs, ARG_mount_point };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_bus, MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_cs, MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_mount_point, MP_ARG_OBJ | MP_ARG_REQUIRED },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // Validate bus object type
    if (!mp_obj_is_type(args[ARG_bus].u_obj, &esp_spi_bus_type)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Expected esp_spi.SPIBus object"));
    }

    // Create object using mp_obj_malloc (modern way)
    esp_sd_obj_t *self = mp_obj_malloc(esp_sd_obj_t, type);
    self->card = NULL;
    self->mounted = false;
    
    // Store bus and CS pin in the object
    self->bus = args[ARG_bus].u_obj;
    self->cs_pin = args[ARG_cs].u_int;
    
    // Copy mount point string
    const char *mount_point_str = mp_obj_str_get_str(args[ARG_mount_point].u_obj);
    size_t len = strlen(mount_point_str);
    self->mount_point = m_new(char, len + 1);
    strcpy(self->mount_point, mount_point_str);

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t esp_sd_mount(mp_obj_t self_in) {
    esp_sd_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (self->mounted) {
        mp_raise_ValueError(MP_ERROR_TEXT("Already mounted"));
    }

    // Validate bus object type (should already be validated in make_new, but double-check)
    if (!mp_obj_is_type(self->bus, &esp_spi_bus_type)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Invalid SPI bus object"));
    }
    
    esp_spi_bus_obj_t *bus = MP_OBJ_TO_PTR(self->bus);
    
    if (!bus->initialized) {
        mp_raise_ValueError(MP_ERROR_TEXT("SPI bus not initialized"));
    }

    spi_host_device_t host_id = bus->host;

    // SD card slot configuration
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = self->cs_pin;
    slot_config.host_id = host_id;

    // Mount configuration
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    // Mount the SD card
    esp_err_t ret = esp_vfs_fat_sdspi_mount(self->mount_point, &host_id, &slot_config, &mount_config, &self->card);
    if (ret != ESP_OK) {
        mp_raise_OSError(ret);
    }

    self->mounted = true;
    ESP_LOGI(TAG, "SD card mounted at %s", self->mount_point);
    sdmmc_card_print_info(stdout, self->card);

    return mp_const_none;
}

static mp_obj_t esp_sd_umount(mp_obj_t self_in) {
    esp_sd_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    if (!self->mounted || !self->card) {
        mp_raise_ValueError(MP_ERROR_TEXT("Not mounted"));
    }

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(self->mount_point, self->card);
    if (ret != ESP_OK) {
        mp_raise_OSError(ret);
    }

    self->card = NULL;
    self->mounted = false;
    ESP_LOGI(TAG, "SD card unmounted from %s", self->mount_point);

    return mp_const_none;
}

static mp_obj_t esp_sd_deinit(mp_obj_t self_in) {
    esp_sd_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    // Unmount if still mounted
    if (self->mounted) {
        esp_sd_umount(self_in);
    }
    
    // Free mount point string
    if (self->mount_point) {
        m_free(self->mount_point);
        self->mount_point = NULL;
    }

    return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(esp_sd_umount_obj, esp_sd_umount);
static MP_DEFINE_CONST_FUN_OBJ_1(esp_sd_mount_obj, esp_sd_mount);
static MP_DEFINE_CONST_FUN_OBJ_1(esp_sd_deinit_obj, esp_sd_deinit);

static const mp_rom_map_elem_t esp_sd_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_mount), MP_ROM_PTR(&esp_sd_mount_obj) },
    { MP_ROM_QSTR(MP_QSTR_umount), MP_ROM_PTR(&esp_sd_umount_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&esp_sd_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(esp_sd_locals_dict, esp_sd_locals_dict_table);

// Modern MicroPython type definition
MP_DEFINE_CONST_OBJ_TYPE(
    esp_sd_type,
    MP_QSTR_SDCard,
    MP_TYPE_FLAG_NONE,
    print, esp_sd_print,
    make_new, esp_sd_make_new,
    locals_dict, &esp_sd_locals_dict
);

static const mp_rom_map_elem_t esp_sd_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_esp_sd) },
    { MP_ROM_QSTR(MP_QSTR_SDCard), MP_ROM_PTR(&esp_sd_type) },
};
static MP_DEFINE_CONST_DICT(esp_sd_module_globals, esp_sd_module_globals_table);

const mp_obj_module_t esp_sd_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&esp_sd_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_esp_sd, esp_sd_module);

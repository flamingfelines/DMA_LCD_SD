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

typedef struct esp_sd_obj_t {
    mp_obj_base_t base;
    sdmmc_card_t *card;
    char *mount_point;
    mp_obj_t bus;           // Store the SPI bus object
    int cs_pin;             // Store the CS pin
    bool mounted;
} esp_sd_obj_t;

// Forward declaration of the type
extern const mp_obj_type_t esp_sd_type;

static void esp_sd_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    esp_sd_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<esp_sd.SDCard mounted=%d mount_point=%s cs=%d>", 
              self->mounted, self->mount_point ? self->mount_point : "None", self->cs_pin);
}

static mp_obj_t esp_sd_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args, mp_map_t *kw_args) {
    enum { ARG_bus, ARG_cs, ARG_mount_point };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_bus, MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_cs, MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_mount_point, MP_ARG_OBJ | MP_ARG_REQUIRED },
    };

    // Parse arguments
    mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed_args);

    // Validate bus object type
    if (!mp_obj_is_type(parsed_args[ARG_bus].u_obj, &esp_spi_bus_type)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Expected esp_spi.SPIBus object"));
    }

    // Create object using mp_obj_malloc (modern way)
    esp_sd_obj_t *self = mp_obj_malloc(esp_sd_obj_t, type);
    self->card = NULL;
    self->mounted = false;
    
    // Store bus and CS pin in the object
    self->bus = parsed_args[ARG_bus].u_obj;
    self->cs_pin = parsed_args[ARG_cs].u_int;
    
    // Copy mount point string
    const char *mount_point_str = mp_obj_str_get_str(parsed_args[ARG_mount_point].u_obj);
    size_t len = strlen(mount_point_str);
    self->mount_point = m_new(char, len + 1);
    strcpy(self->mount_point, mount_point_str);

    return MP_OBJ_FROM_PTR(self);
}

// Custom ESP-IDF SD card mount function (renamed to avoid conflicts)
static mp_obj_t esp_sd_card_mount(mp_obj_t self_in) {
    esp_sd_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (self->mounted) {
        mp_raise_ValueError(MP_ERROR_TEXT("SD card already mounted"));
    }

    // Validate bus object type
    if (!mp_obj_is_type(self->bus, &esp_spi_bus_type)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Invalid SPI bus object"));
    }
    
    esp_spi_bus_obj_t *bus = MP_OBJ_TO_PTR(self->bus);
    
    if (!bus->initialized) {
        mp_raise_ValueError(MP_ERROR_TEXT("SPI bus not initialized"));
    }

    spi_host_device_t host_id = bus->host;

    // SD card slot configuration for SPI mode
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = self->cs_pin;
    slot_config.host_id = host_id;

    // Create proper sdmmc_host_t structure for SPI mode
    sdmmc_host_t host_config = SDSPI_HOST_DEFAULT();
    host_config.slot = host_id;

    // ESP-IDF VFS FAT mount configuration
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    // Use ESP-IDF's VFS FAT mount function (not MicroPython's VFS)
    esp_err_t ret = esp_vfs_fat_sdspi_mount(self->mount_point, &host_config, &slot_config, &mount_config, &self->card);
    if (ret != ESP_OK) {
        const char *error_msg;
        switch (ret) {
            case ESP_ERR_NO_MEM:
                error_msg = "Out of memory";
                break;
            case ESP_ERR_INVALID_STATE:
                error_msg = "Invalid state";
                break;
            case ESP_ERR_TIMEOUT:
                error_msg = "SD card timeout";
                break;
            default:
                error_msg = "SD card mount failed";
                break;
        }
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("%s (ESP error: 0x%x)"), error_msg, ret);
    }

    self->mounted = true;
    ESP_LOGI(TAG, "ESP-IDF SD card mounted at %s", self->mount_point);
    
    // Print card information if available
    if (self->card) {
        sdmmc_card_print_info(stdout, self->card);
    }

    return mp_const_none;
}

// Custom ESP-IDF SD card unmount function (renamed to avoid conflicts)
static mp_obj_t esp_sd_card_unmount(mp_obj_t self_in) {
    esp_sd_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    if (!self->mounted || !self->card) {
        mp_raise_ValueError(MP_ERROR_TEXT("SD card not mounted"));
    }

    // Use ESP-IDF's VFS FAT unmount function
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(self->mount_point, self->card);
    if (ret != ESP_OK) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("SD card unmount failed (ESP error: 0x%x)"), ret);
    }

    self->card = NULL;
    self->mounted = false;
    ESP_LOGI(TAG, "ESP-IDF SD card unmounted from %s", self->mount_point);

    return mp_const_none;
}

// Get SD card info (custom function using ESP-IDF)
static mp_obj_t esp_sd_card_info(mp_obj_t self_in) {
    esp_sd_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    if (!self->mounted || !self->card) {
        mp_raise_ValueError(MP_ERROR_TEXT("SD card not mounted"));
    }

    // Create a dictionary with card information
    mp_obj_t info_dict = mp_obj_new_dict(6);
    
    // Card name
    mp_obj_dict_store(info_dict, MP_OBJ_NEW_QSTR(MP_QSTR_name), 
                     mp_obj_new_str(self->card->cid.name, strlen(self->card->cid.name)));
    
    // Card capacity in MB
    uint64_t capacity_mb = ((uint64_t)self->card->csd.capacity) * self->card->csd.sector_size / (1024 * 1024);
    mp_obj_dict_store(info_dict, MP_OBJ_NEW_QSTR(MP_QSTR_capacity_mb), 
                     mp_obj_new_int(capacity_mb));
    
    // Sector size
    mp_obj_dict_store(info_dict, MP_OBJ_NEW_QSTR(MP_QSTR_sector_size), 
                     mp_obj_new_int(self->card->csd.sector_size));
    
    // Card type
    const char *card_type_str;
    switch (self->card->csd.card_type) {
        case SDMMC_CARD_TYPE_SDSC: card_type_str = "SDSC"; break;
        case SDMMC_CARD_TYPE_SDHC: card_type_str = "SDHC"; break;
        case SDMMC_CARD_TYPE_SDXC: card_type_str = "SDXC"; break;
        default: card_type_str = "Unknown"; break;
    }
    mp_obj_dict_store(info_dict, MP_OBJ_NEW_QSTR(MP_QSTR_type), 
                     mp_obj_new_str(card_type_str, strlen(card_type_str)));
    
    // Speed
    mp_obj_dict_store(info_dict, MP_OBJ_NEW_QSTR(MP_QSTR_max_freq_khz), 
                     mp_obj_new_int(self->card->max_freq_khz));
    
    // Mount point
    mp_obj_dict_store(info_dict, MP_OBJ_NEW_QSTR(MP_QSTR_mount_point), 
                     mp_obj_new_str(self->mount_point, strlen(self->mount_point)));
    
    return info_dict;
}

// Check if SD card is mounted
static mp_obj_t esp_sd_is_mounted(mp_obj_t self_in) {
    esp_sd_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(self->mounted);
}

// Cleanup function (renamed for clarity)
static mp_obj_t esp_sd_card_deinit(mp_obj_t self_in) {
    esp_sd_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    // Unmount if still mounted
    if (self->mounted) {
        esp_sd_card_unmount(self_in);
    }
    
    // Free mount point string
    if (self->mount_point) {
        m_free(self->mount_point);
        self->mount_point = NULL;
    }

    return mp_const_none;
}

// Define method objects with clear naming
static MP_DEFINE_CONST_FUN_OBJ_1(esp_sd_card_mount_obj, esp_sd_card_mount);
static MP_DEFINE_CONST_FUN_OBJ_1(esp_sd_card_unmount_obj, esp_sd_card_unmount);
static MP_DEFINE_CONST_FUN_OBJ_1(esp_sd_card_info_obj, esp_sd_card_info);
static MP_DEFINE_CONST_FUN_OBJ_1(esp_sd_is_mounted_obj, esp_sd_is_mounted);
static MP_DEFINE_CONST_FUN_OBJ_1(esp_sd_card_deinit_obj, esp_sd_card_deinit);

// Local dictionary for the SDCard class
static const mp_rom_map_elem_t esp_sd_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_mount), MP_ROM_PTR(&esp_sd_card_mount_obj) },
    { MP_ROM_QSTR(MP_QSTR_unmount), MP_ROM_PTR(&esp_sd_card_unmount_obj) },
    { MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&esp_sd_card_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_mounted), MP_ROM_PTR(&esp_sd_is_mounted_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&esp_sd_card_deinit_obj) },
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

// Module globals
static const mp_rom_map_elem_t esp_sd_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_esp_sd) },
    { MP_ROM_QSTR(MP_QSTR_SDCard), MP_ROM_PTR(&esp_sd_type) },
};
static MP_DEFINE_CONST_DICT(esp_sd_module_globals, esp_sd_module_globals_table);

// Module definition
const mp_obj_module_t esp_sd_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&esp_sd_module_globals,
};

// Register the module with MicroPython
MP_REGISTER_MODULE(MP_QSTR_esp_sd, esp_sd_module);

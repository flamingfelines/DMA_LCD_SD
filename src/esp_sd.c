// Modified esp_sd.c - Raw block device without ESP-IDF VFS/FAT
#include "py/obj.h"
#include "py/runtime.h"
#include "py/mperrno.h"
#include "py/mpconfig.h"
#include <string.h>
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_spi.h"

static const char *TAG = "esp_sd";

typedef struct esp_sd_obj_t {
    mp_obj_base_t base;
    sdmmc_card_t *card;
    mp_obj_t bus;           
    int cs_pin;             
    bool initialized;
    uint32_t block_count;
    uint32_t block_size;
    sdspi_dev_handle_t spi_handle;
} esp_sd_obj_t;

extern const mp_obj_type_t esp_sd_type;

static void esp_sd_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    esp_sd_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<esp_sd.SDCard initialized=%d blocks=%lu block_size=%lu cs=%d>", 
              self->initialized, self->block_count, self->block_size, self->cs_pin);
}

static mp_obj_t esp_sd_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args, mp_map_t *kw_args) {
    enum { ARG_bus, ARG_cs };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_bus, MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_cs, MP_ARG_INT | MP_ARG_REQUIRED },
    };
    mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed_args);
    
    // Validate bus object first
    if (!mp_obj_is_type(parsed_args[ARG_bus].u_obj, &esp_spi_bus_type)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Expected esp_spi.SPIBus object"));
    }

    // Validate CS pin range (adjust range as needed for your platform)
    int cs_pin = parsed_args[ARG_cs].u_int;
    if (cs_pin < 0 || cs_pin > 39) {  // ESP32 GPIO range example
        mp_raise_ValueError(MP_ERROR_TEXT("Invalid CS pin number"));
    }
    
    // Now safe to log
    ESP_LOGI(TAG, "esp_sd_make_new called, bus ptr=%p, cs=%d",
             parsed_args[ARG_bus].u_obj, cs_pin);

    esp_sd_obj_t *self = mp_obj_malloc(esp_sd_obj_t, type);
    self->card = NULL;
    self->initialized = false;
    self->bus = parsed_args[ARG_bus].u_obj;
    self->cs_pin = parsed_args[ARG_cs].u_int;
    self->block_count = 0;
    self->block_size = 512;  // Standard SD block size
    self->spi_handle = NULL;   // Invalid handle initially

    return MP_OBJ_FROM_PTR(self);
}

// Initialize SD card (no VFS mounting, just card initialization)
static mp_obj_t esp_sd_init(mp_obj_t self_in) {
    esp_sd_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (self->initialized) {
        return mp_const_none;
    }

    if (!mp_obj_is_type(self->bus, &esp_spi_bus_type)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Invalid SPI bus object"));
    }
    
    esp_spi_bus_obj_t *bus = MP_OBJ_TO_PTR(self->bus);
    
    if (!bus->initialized) {
        mp_raise_ValueError(MP_ERROR_TEXT("SPI bus not initialized"));
    }

    // SD card slot configuration for SPI mode
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = self->cs_pin;
    slot_config.host_id = bus->host;
    
    // Initialize SPI device handle
    esp_err_t ret = sdspi_host_init_device(&slot_config, &self->spi_handle);
    if (ret != ESP_OK) {
        free(self->card);
        self->card = NULL;
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("SD SPI init failed (ESP error: 0x%x)"), ret);
    }

    // Create sdmmc_host_t structure for SPI mode
    sdmmc_host_t host_config = SDSPI_HOST_DEFAULT();
    host_config.slot = bus->host;

    // Allocate card structure
    self->card = malloc(sizeof(sdmmc_card_t));
    if (self->card == NULL) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Out of memory"));
    }

    // Initialize card protocol
    ret = sdmmc_card_init(&host_config, self->card);
    if (ret != ESP_OK) {
        sdspi_host_remove_device(self->spi_handle);
        free(self->card);
        self->card = NULL;
        self->spi_handle = -1;
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("SD card init failed (ESP error: 0x%x)"), ret);
    }

    // Store card info
    self->block_count = self->card->csd.capacity;
    self->block_size = self->card->csd.sector_size;
    self->initialized = true;

    ESP_LOGI(TAG, "SD card initialized: %lu blocks of %lu bytes", self->block_count, self->block_size);
    
    return mp_const_none;
}

// Raw block read (for MicroPython VFS)
static mp_obj_t esp_sd_readblocks(mp_obj_t self_in, mp_obj_t block_num_obj, mp_obj_t buf_obj) {
    esp_sd_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    if (!self->initialized || !self->card) {
        mp_raise_ValueError(MP_ERROR_TEXT("SD card not initialized"));
    }

    uint32_t block_num = mp_obj_get_int(block_num_obj);
    mp_buffer_info_t buf_info;
    mp_get_buffer_raise(buf_obj, &buf_info, MP_BUFFER_WRITE);

    // Check bounds
    uint32_t num_blocks = buf_info.len / self->block_size;
    if (block_num + num_blocks > self->block_count) {
        mp_raise_ValueError(MP_ERROR_TEXT("Block read out of range"));
    }

    // Read blocks directly from SD card
    esp_err_t ret = sdmmc_read_sectors(self->card, buf_info.buf, block_num, num_blocks);
    if (ret != ESP_OK) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("SD read failed (ESP error: 0x%x)"), ret);
    }

    return mp_const_none;
}

// Raw block write (for MicroPython VFS)
static mp_obj_t esp_sd_writeblocks(mp_obj_t self_in, mp_obj_t block_num_obj, mp_obj_t buf_obj) {
    esp_sd_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    if (!self->initialized || !self->card) {
        mp_raise_ValueError(MP_ERROR_TEXT("SD card not initialized"));
    }

    uint32_t block_num = mp_obj_get_int(block_num_obj);
    mp_buffer_info_t buf_info;
    mp_get_buffer_raise(buf_obj, &buf_info, MP_BUFFER_READ);

    // Check bounds
    uint32_t num_blocks = buf_info.len / self->block_size;
    if (block_num + num_blocks > self->block_count) {
        mp_raise_ValueError(MP_ERROR_TEXT("Block write out of range"));
    }

    // Write blocks directly to SD card
    esp_err_t ret = sdmmc_write_sectors(self->card, buf_info.buf, block_num, num_blocks);
    if (ret != ESP_OK) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("SD write failed (ESP error: 0x%x)"), ret);
    }

    return mp_const_none;
}

// Get block count
static mp_obj_t esp_sd_count(mp_obj_t self_in) {
    esp_sd_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    if (!self->initialized) {
        mp_raise_ValueError(MP_ERROR_TEXT("SD card not initialized"));
    }
    
    return mp_obj_new_int(self->block_count);
}

// ioctl implementation for MicroPython VFS
static mp_obj_t esp_sd_ioctl(mp_obj_t self_in, mp_obj_t op_obj, mp_obj_t arg_obj) {
    esp_sd_obj_t *self = MP_OBJ_TO_PTR(self_in);
    int op = mp_obj_get_int(op_obj);
    
    switch (op) {
        case 4: // BP_IOCTL_SEC_COUNT
            return mp_obj_new_int(self->block_count);
        case 5: // BP_IOCTL_SEC_SIZE
            return mp_obj_new_int(self->block_size);
        default:
            return mp_obj_new_int(0);
    }
}

// Cleanup
static mp_obj_t esp_sd_deinit(mp_obj_t self_in) {
    esp_sd_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    if (self->spi_handle != -1) {
        sdspi_host_remove_device(self->spi_handle);
        self->spi_handle = -1;
    }
    
    if (self->card) {
        free(self->card);
        self->card = NULL;
    }
    
    self->initialized = false;
    return mp_const_none;
}

// Method definitions
static MP_DEFINE_CONST_FUN_OBJ_1(esp_sd_init_obj, esp_sd_init);
static MP_DEFINE_CONST_FUN_OBJ_3(esp_sd_readblocks_obj, esp_sd_readblocks);
static MP_DEFINE_CONST_FUN_OBJ_3(esp_sd_writeblocks_obj, esp_sd_writeblocks);
static MP_DEFINE_CONST_FUN_OBJ_1(esp_sd_count_obj, esp_sd_count);
static MP_DEFINE_CONST_FUN_OBJ_3(esp_sd_ioctl_obj, esp_sd_ioctl);
static MP_DEFINE_CONST_FUN_OBJ_1(esp_sd_deinit_obj, esp_sd_deinit);

// Local dictionary
static const mp_rom_map_elem_t esp_sd_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&esp_sd_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_readblocks), MP_ROM_PTR(&esp_sd_readblocks_obj) },
    { MP_ROM_QSTR(MP_QSTR_writeblocks), MP_ROM_PTR(&esp_sd_writeblocks_obj) },
    { MP_ROM_QSTR(MP_QSTR_count), MP_ROM_PTR(&esp_sd_count_obj) },
    { MP_ROM_QSTR(MP_QSTR_ioctl), MP_ROM_PTR(&esp_sd_ioctl_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&esp_sd_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(esp_sd_locals_dict, esp_sd_locals_dict_table);

// Type definition
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

const mp_obj_module_t esp_sd_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&esp_sd_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_esp_sd, esp_sd_module);

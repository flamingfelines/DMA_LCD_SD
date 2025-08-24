#include "esp_spi.h"
#include "py/runtime.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "py/obj.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char* TAG = "esp_spi";

static void enable_miso_pullup(int miso_gpio) {
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << miso_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "Enabled pull-up on GPIO%d (MISO)", miso_gpio);
}

static void esp_spi_bus_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    esp_spi_bus_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<esp_spi.SPIBus miso=%d mosi=%d sclk=%d initialized=%d>",
        self->miso_io_num, self->mosi_io_num, self->sclk_io_num, self->initialized);
}

// Fixed esp_spi_bus_make_new function for ESP-IDF 5.4.2 compatibility
static mp_obj_t esp_spi_bus_make_new(const mp_obj_type_t *type,
                                    size_t n_args, size_t n_kw,
                                    const mp_obj_t *args) {
    
    // Use simple positional argument parsing instead of mp_arg_parse_all_kw_array
    if (n_args < 3 || n_args > 4) {
        mp_raise_TypeError(MP_ERROR_TEXT("SPIBus requires 3-4 arguments: miso, mosi, sclk [, host]"));
    }
    
    // Get required arguments
    int miso_pin = mp_obj_get_int(args[0]);
    int mosi_pin = mp_obj_get_int(args[1]);
    int sclk_pin = mp_obj_get_int(args[2]);
    
    // Optional host argument (defaults to SPI2_HOST)
    int host = (n_args >= 4) ? mp_obj_get_int(args[3]) : SPI2_HOST;
    
    // Validate GPIO pins for ESP32-S3
    if (miso_pin < -1 || miso_pin > 48 || 
        mosi_pin < -1 || mosi_pin > 48 || 
        sclk_pin < -1 || sclk_pin > 48) {
        mp_raise_ValueError(MP_ERROR_TEXT("Invalid GPIO pin number"));
    }
    enable_miso_pullup(miso_pin);
        
    // Validate SPI host
    if (host < SPI1_HOST || host > SPI3_HOST) {
        mp_raise_ValueError(MP_ERROR_TEXT("Invalid SPI host"));
    }

    esp_spi_bus_obj_t *self = mp_obj_malloc(esp_spi_bus_obj_t, type);
    self->base.type = &esp_spi_bus_type;
    self->miso_io_num = miso_pin;
    self->mosi_io_num = mosi_pin;
    self->sclk_io_num = sclk_pin;
    self->host = host;
    self->initialized = false;

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t esp_spi_bus_init(mp_obj_t self_in) {
    esp_spi_bus_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (self->initialized) {
        return mp_const_none; // Already initialized
    }

    spi_bus_config_t buscfg = {
        .miso_io_num = self->miso_io_num,
        .mosi_io_num = self->mosi_io_num,
        .sclk_io_num = self->sclk_io_num,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 1024 * 1024, // ~1MB adjust as needed
    };

    esp_err_t ret = spi_bus_initialize(self->host, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("spi_bus_initialize failed"));
    }

    self->initialized = true;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esp_spi_bus_init_obj, esp_spi_bus_init);

// Device object print
static void esp_spi_device_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    esp_spi_device_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<esp_spi.SPIDevice %p>", self->spi_dev_handle);
}

static const mp_rom_map_elem_t esp_spi_bus_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&esp_spi_bus_init_obj) },
};
static MP_DEFINE_CONST_DICT(esp_spi_bus_locals_dict, esp_spi_bus_locals_dict_table);

// Modern MicroPython type definition using slots
MP_DEFINE_CONST_OBJ_TYPE(
    esp_spi_device_type,
    MP_QSTR_SPIDevice,
    MP_TYPE_FLAG_NONE,
    print, esp_spi_device_print
);

MP_DEFINE_CONST_OBJ_TYPE(
    esp_spi_bus_type,
    MP_QSTR_SPIBus,
    MP_TYPE_FLAG_NONE,
    print, esp_spi_bus_print,
    make_new, esp_spi_bus_make_new,
    locals_dict, &esp_spi_bus_locals_dict
);

// Module globals table
static const mp_rom_map_elem_t esp_spi_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_esp_spi) },
    { MP_ROM_QSTR(MP_QSTR_SPIBus), MP_ROM_PTR(&esp_spi_bus_type) },
    { MP_ROM_QSTR(MP_QSTR_SPIDevice), MP_ROM_PTR(&esp_spi_device_type) },
};
static MP_DEFINE_CONST_DICT(esp_spi_module_globals, esp_spi_module_globals_table);

// Define the module
const mp_obj_module_t mp_module_esp_spi = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&esp_spi_module_globals,
};

// Register the module so MicroPython can find it
MP_REGISTER_MODULE(MP_QSTR_esp_spi, mp_module_esp_spi);

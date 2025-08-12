#include "esp_spi.h"
#include "py/runtime.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "py/obj.h"
#include "driver/gpio.h"

static void esp_spi_bus_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    esp_spi_bus_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<esp_spi.SPIBus miso=%d mosi=%d sclk=%d initialized=%d>",
        self->miso_io_num, self->mosi_io_num, self->sclk_io_num, self->initialized);
}

static mp_obj_t esp_spi_bus_make_new(const mp_obj_type_t *type,
                                    size_t n_args, size_t n_kw,
                                    const mp_obj_t *args) {
    enum { ARG_miso, ARG_mosi, ARG_sclk, ARG_host };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_miso, MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_mosi, MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_sclk, MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_host, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = SPI2_HOST} },
    };

    mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, args, n_kw, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed_args);

    esp_spi_bus_obj_t *self = mp_obj_malloc(esp_spi_bus_obj_t, type);
    self->base.type = &esp_spi_bus_type;
    self->miso_io_num = parsed_args[ARG_miso].u_int;
    self->mosi_io_num = parsed_args[ARG_mosi].u_int;
    self->sclk_io_num = parsed_args[ARG_sclk].u_int;
    self->host = parsed_args[ARG_host].u_int;
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
        .max_transfer_sz = 240 * 240 * 2 + 8, // max size for your framebuffer, adjust as needed
    };

    esp_err_t ret = spi_bus_initialize(self->host, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("spi_bus_initialize failed"));
    }

    self->initialized = true;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esp_spi_bus_init_obj, esp_spi_bus_init);

static mp_obj_t esp_spi_bus_add_device(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    esp_spi_bus_obj_t *self = MP_OBJ_TO_PTR(args[0]);

    if (!self->initialized) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("spi_bus_initialize failed"));
    }

    enum {
        ARG_cs,
        ARG_ds,
        ARG_freq,
        ARG_mode,
    };

    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_cs, MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_ds, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
        { MP_QSTR_freq, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 40000000} },
        { MP_QSTR_mode, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
    };

    mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed_args);

    spi_device_interface_config_t devcfg = {
        .command_bits = 0,
        .address_bits = 0,
        .dummy_bits = 0,
        .mode = parsed_args[ARG_mode].u_int,
        .duty_cycle_pos = 128, // default
        .cs_ena_posttrans = 3,
        .clock_speed_hz = parsed_args[ARG_freq].u_int,
        .input_delay_ns = 0,
        .spics_io_num = parsed_args[ARG_cs].u_int,
        .flags = 0,
        .queue_size = 7,
        .pre_cb = NULL,
        .post_cb = NULL,
    };

    // If ds pin is provided and valid, configure it here (optional):
    if (parsed_args[ARG_ds].u_int >= 0) {
        // TODO: If needed, you can set up GPIO here or pass to devcfg.flags/custom callbacks
        // For now, just set it as a comment or store for future use
    }

    spi_device_handle_t spi_dev_handle;
    esp_err_t ret = spi_bus_add_device(self->host, &devcfg, &spi_dev_handle);
    if (ret != ESP_OK) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("spi_bus_add_device failed"));
    }

    esp_spi_device_obj_t *dev = mp_obj_malloc(esp_spi_device_obj_t, &esp_spi_device_type);
    dev->spi_dev_handle = spi_dev_handle;

    return MP_OBJ_FROM_PTR(dev);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esp_spi_bus_add_device_obj, 1, esp_spi_bus_add_device);

// Device object print
static void esp_spi_device_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    esp_spi_device_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<esp_spi.SPIDevice %p>", self->spi_dev_handle);
}

static const mp_rom_map_elem_t esp_spi_bus_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&esp_spi_bus_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_add_device), MP_ROM_PTR(&esp_spi_bus_add_device_obj) },
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

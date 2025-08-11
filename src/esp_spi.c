#include "esp_spi.h"
#include "py/runtime.h"
#include "driver/spi_master.h"
#include "esp_err.h"

STATIC void esp_spi_bus_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    esp_spi_bus_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<esp_spi.SPIBus miso=%d mosi=%d sclk=%d initialized=%d>",
        self->miso_io_num, self->mosi_io_num, self->sclk_io_num, self->initialized);
}

STATIC mp_obj_t esp_spi_bus_make_new(const mp_obj_type_t *type,
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

STATIC mp_obj_t esp_spi_bus_init(mp_obj_t self_in) {
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
        mp_raise_msg(&mp_type_OSError, "spi_bus_initialize failed");
    }

    self->initialized = true;
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(esp_spi_bus_init_obj, esp_spi_bus_init);

STATIC const mp_rom_map_elem_t esp_spi_bus_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&esp_spi_bus_init_obj) },
};
STATIC MP_DEFINE_CONST_DICT(esp_spi_bus_locals_dict, esp_spi_bus_locals_dict_table);

const mp_obj_type_t esp_spi_bus_type = {
    { &mp_type_type },
    .name = MP_QSTR_SPIBus,
    .print = esp_spi_bus_print,
    .make_new = esp_spi_bus_make_new,
    .locals_dict = (mp_obj_dict_t *)&esp_spi_bus_locals_dict,
};

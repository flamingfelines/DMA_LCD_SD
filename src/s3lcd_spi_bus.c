#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "soc/soc_caps.h"
#include "driver/gpio.h"

#include "mphalport.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/mpconfig.h"
#include "py/gc.h"

#include "s3lcd_spi_bus.h"
#include "esp_spi.h"
#include <string.h>


static void s3lcd_spi_bus_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void) kind;
    s3lcd_spi_bus_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<SPI_BUS %s, dc=%d, cs=%d, spi_mode=%d, pclk=%d, lcd_cmd_bits=%d, "
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
                     "lcd_param_bits=%d, dc_as_cmd_phase=%d, dc_low_on_data=%d, "
#else
                     "lcd_param_bits=%d, dc_low_on_data=%d, "
#endif
                     "octal_mode=%d, lsb_first=%d, swap_color_bytes=%d>",

        self->name,
        self->dc_gpio_num,
        self->cs_gpio_num,
        self->spi_mode,
        self->pclk_hz,
        self->lcd_cmd_bits,
        self->lcd_param_bits,
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
        self->flags.dc_as_cmd_phase,
#endif
        self->flags.dc_low_on_data,
        self->flags.octal_mode,
        self->flags.lsb_first,
        self->flags.swap_color_bytes);
}

static mp_obj_t s3lcd_spi_bus_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args)
{
    enum {
        ARG_esp_spi_bus,        // esp_spi.SPIBus object
        ARG_spi_host,           // SPI host used in SPI object above
        ARG_dc,                 // GPIO used to select the D/C line, set this to -1 if the D/C line not controlled by manually pulling high/low GPIO
        ARG_cs,                 // GPIO used for CS line
        ARG_spi_mode,           // Traditional SPI mode (0~3)
        ARG_pclk_hz,            // Frequency of pixel clock
        ARG_lcd_cmd_bits,       // Bit-width of LCD command
        ARG_lcd_param_bits,     // Bit-width of LCD parameter
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
        ARG_dc_as_cmd_phase,    // D/C line value is encoded into SPI transaction command phase
#endif
        ARG_dc_low_on_data,     // If this flag is enabled, DC line = 0 means transfer data, DC line = 1 means transfer command; vice versa
        ARG_octal_mode,         // transmit with octal mode (8 data lines), this mode is used to simulate Intel 8080 timing
        ARG_lsb_first,          // transmit LSB bit first
        ARG_swap_color_bytes,   // Swap data byte order
    };

    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_bus,              MP_ARG_OBJ  | MP_ARG_REQUIRED                      },
        { MP_QSTR_spi_host,         MP_ARG_INT  | MP_ARG_REQUIRED                      },
        { MP_QSTR_dc,               MP_ARG_INT  | MP_ARG_REQUIRED                      },
        { MP_QSTR_cs,               MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_int = -1       } },
        { MP_QSTR_spi_mode,         MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_int = 0        } },
        { MP_QSTR_pclk,             MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_int = 40000000 } },
        { MP_QSTR_lcd_cmd_bits,     MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_int = 8        } },
        { MP_QSTR_lcd_param_bits,   MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_int = 8        } },
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
        { MP_QSTR_dc_as_cmd_phase,  MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_int = 0        } },
#endif
        { MP_QSTR_dc_low_on_data,   MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_int = 0        } },
        { MP_QSTR_octal_mode,       MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false   } },
        { MP_QSTR_lsb_first,        MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false   } },
        { MP_QSTR_swap_color_bytes, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false   } },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    //Validate spi object
    mp_obj_t bus_obj = args[ARG_esp_spi_bus].u_obj;
    if (!mp_obj_is_type(bus_obj, &esp_spi_bus_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("bus must be an esp_spi.SPIBus object"));
    }

    // Get the SPI bus object to verify it's initialized
    esp_spi_bus_obj_t *spi_bus = MP_OBJ_TO_PTR(bus_obj);
    if (!spi_bus->initialized) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("SPI bus not initialized"));
    }

    s3lcd_spi_bus_obj_t *self = mp_obj_malloc(s3lcd_spi_bus_obj_t, type);
    
    self->base.type = &s3lcd_spi_bus_type;
    self->name = "s3lcd_spi";
    self->bus_obj = bus_obj;
    self->spi_host = args[ARG_spi_host].u_int;
    self->dc_gpio_num = args[ARG_dc].u_int;
    self->cs_gpio_num = args[ARG_cs].u_int;
    self->spi_mode = args[ARG_spi_mode].u_int;
    self->pclk_hz = args[ARG_pclk_hz].u_int;
    self->lcd_cmd_bits = args[ARG_lcd_cmd_bits].u_int;
    self->lcd_param_bits = args[ARG_lcd_param_bits].u_int;
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    self->flags.dc_as_cmd_phase = args[ARG_dc_as_cmd_phase].u_int;
#endif
    self->flags.dc_low_on_data = args[ARG_dc_low_on_data].u_int;
    self->flags.octal_mode = args[ARG_octal_mode].u_bool;
    self->flags.lsb_first = args[ARG_lsb_first].u_bool;
    self->flags.swap_color_bytes = args[ARG_swap_color_bytes].u_bool;

    // Create the LCD panel IO handle using the existing shared SPI bus
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = self->dc_gpio_num,
        .cs_gpio_num = self->cs_gpio_num,
        .pclk_hz = self->pclk_hz,
        .spi_mode = self->spi_mode,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = self->lcd_cmd_bits,
        .lcd_param_bits = self->lcd_param_bits,
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
        .flags.dc_as_cmd_phase = self->flags.dc_as_cmd_phase,
#endif
        .flags.dc_low_on_data = self->flags.dc_low_on_data,
        .flags.octal_mode = self->flags.octal_mode,
        .flags.lsb_first = self->flags.lsb_first
    };

    // Create the panel IO handle - this is the key step!
    // Cast spi_host to esp_lcd_spi_bus_handle_t as shown in ESP-IDF docs
    esp_err_t ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)self->spi_host, &io_config, &self->io_handle);
    if (ret != ESP_OK) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to create LCD panel IO"));
    }

    // Initialize spi_dev as NULL - not needed anymore since we have proper IO handle
    self->spi_dev = NULL;

    return MP_OBJ_FROM_PTR(self);
}

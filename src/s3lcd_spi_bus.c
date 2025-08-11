#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "soc/soc_caps.h"
#include "driver/gpio.h"
#include "mphalport.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/gc.h"
#include "s3lcd_spi_bus.h"
#include "machine_hw_spi.h"  // Added for machine.SPI object access
#include <string.h>

// External declaration for machine.SPI type
extern const mp_obj_type_t machine_spi_type;

static void s3lcd_spi_bus_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void) kind;
    s3lcd_spi_bus_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    // Get SPI info from machine.SPI object
    machine_hw_spi_obj_t *machine_spi = MP_OBJ_TO_PTR(self->machine_spi_obj);
    
    mp_printf(print, "<SPI %s, machine_spi=%p, dc=%d, cs=%d, spi_mode=%d, pclk=%d, lcd_cmd_bits=%d, "
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
                     "lcd_param_bits=%d, dc_as_cmd_phase=%d, dc_low_on_data=%d, "
#else
                     "lcd_param_bits=%d, dc_low_on_data=%d, "
#endif
                     "octal_mode=%d, lsb_first=%d, swap_color_bytes=%d>",
        self->name,
        machine_spi,              // Reference to machine.SPI object
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

///
/// spi_bus - Configure a SPI bus for shared use with machine.SPI.
///
/// Parameters:
///   - spi: machine.SPI object (required) - the shared SPI bus instance
///   - dc: GPIO used to select the D/C line, set this to -1 if the D/C line not controlled by manually pulling high/low GPIO
///   - cs: GPIO used for CS line (optional, defaults to -1 for no CS control)
///   - spi_mode: Traditional SPI mode (0~3, defaults to 0)
///   - pclk: Frequency of pixel clock in Hz (defaults to 40MHz)
///   - lcd_cmd_bits: Bit-width of LCD command (defaults to 8)
///   - lcd_param_bits: Bit-width of LCD parameter (defaults to 8)
///   - dc_as_cmd_phase: D/C line value is encoded into SPI transaction command phase (ESP-IDF < 5.0 only)
///   - dc_low_on_data: If this flag is enabled, DC line = 0 means transfer data, DC line = 1 means transfer command; vice versa
///   - octal_mode: transmit with octal mode (8 data lines), this mode is used to simulate Intel 8080 timing
///   - lsb_first: transmit LSB bit first
///   - swap_color_bytes: (bool) Swap data byte order
///
/// Example usage:
/// ```python
/// from machine import Pin, SPI
/// import s3lcd
/// 
/// # Create shared SPI bus
/// spi = SPI(2, baudrate=40000000, sck=Pin(18), mosi=Pin(23), miso=Pin(19))
/// 
/// # Create display bus using shared SPI
/// bus = s3lcd.SPI_BUS(spi=spi, dc=Pin(2), cs=Pin(5))
/// 
/// # SD card can share the same SPI bus
/// import sdcard
/// sd = sdcard.SDCard(spi, Pin(15))  # Different CS pin
/// ```

static mp_obj_t s3lcd_spi_bus_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args)
{
    enum {
        ARG_spi,                // machine.SPI object to use
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
        { MP_QSTR_spi,              MP_ARG_OBJ  | MP_ARG_REQUIRED,                     },
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

    // Extract and validate machine.SPI object
    mp_obj_t spi_obj = args[ARG_spi].u_obj;
    if (!mp_obj_is_type(spi_obj, &machine_spi_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("spi must be a machine.SPI object"));
    }

    // Get the SPI host from machine.SPI object
    machine_hw_spi_obj_t *machine_spi = MP_OBJ_TO_PTR(spi_obj);
    spi_host_device_t spi_host = machine_spi->host;

    // create new spi_bus object
    s3lcd_spi_bus_obj_t *self = m_new_obj(s3lcd_spi_bus_obj_t);
    self->base.type = &s3lcd_spi_bus_type;
    self->name = "s3lcd_spi";
    
    // Store reference to machine.SPI object to keep it alive
    self->machine_spi_obj = spi_obj;
    
    // Store other parameters
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
    self->flags.octal_mode = args[ARG_octal_mode].u_int;
    self->flags.lsb_first = args[ARG_lsb_first].u_bool;
    self->flags.swap_color_bytes = args[ARG_swap_color_bytes].u_bool;

    // Configure ESP-LCD SPI panel I/O using the existing SPI bus
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = self->dc_gpio_num,
        .cs_gpio_num = self->cs_gpio_num,
        .pclk_hz = self->pclk_hz,
        .lcd_cmd_bits = self->lcd_cmd_bits,
        .lcd_param_bits = self->lcd_param_bits,
        .spi_mode = self->spi_mode,
        .trans_queue_depth = 10,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .flags = {
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
            .dc_as_cmd_phase = self->flags.dc_as_cmd_phase,
#endif
            .dc_low_on_data = self->flags.dc_low_on_data,
            .octal_mode = self->flags.octal_mode,
            .lsb_first = self->flags.lsb_first,
        }
    };

    // Initialize ESP-LCD panel I/O with the existing SPI host
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(spi_host, &io_config, &self->io_handle));

    return MP_OBJ_FROM_PTR(self);
}

static const mp_rom_map_elem_t s3lcd_spi_bus_locals_dict_table[] = {
};
static MP_DEFINE_CONST_DICT(s3lcd_spi_bus_locals_dict, s3lcd_spi_bus_locals_dict_table);

#if MICROPY_OBJ_TYPE_REPR == MICROPY_OBJ_TYPE_REPR_SLOT_INDEX

MP_DEFINE_CONST_OBJ_TYPE(
    s3lcd_spi_bus_type,
    MP_QSTR_SPI_BUS,
    MP_TYPE_FLAG_NONE,
    print, s3lcd_spi_bus_print,
    make_new, s3lcd_spi_bus_make_new,
    locals_dict, &s3lcd_spi_bus_locals_dict);

#else

const mp_obj_type_t s3lcd_spi_bus_type = {
    {&mp_type_type},
    .name = MP_QSTR_SPI_BUS,
    .print = s3lcd_spi_bus_print,
    .make_new = s3lcd_spi_bus_make_new,
    .locals_dict = (mp_obj_dict_t *)&s3lcd_spi_bus_locals_dict,
};

#endif

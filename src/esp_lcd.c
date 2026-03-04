/*
 * Copyright (c) 2023 Russ Hughes
 * Copyright (c) 2026 FlamingFelines
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE OF ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "soc/soc_caps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mphalport.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/mpconfig.h"
#include "py/gc.h"
#include "esp_lcd.h"
#include "esp_spi.h"
#include "mpfile.h"
#include "pngenc/pngenc.h"
#include <string.h>
#include <stdbool.h>

// ── DMA completion flag ───────────────────────────────────────────────────────

volatile bool lcd_panel_active = false;
bool lcd_panel_done(esp_lcd_panel_io_handle_t panel_io,
                    esp_lcd_panel_io_event_data_t *edata,
                    void *user_ctx) {
    lcd_panel_active = false;
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// ── SPI_BUS ──────────────────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

static void esp_lcd_spi_bus_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    esp_lcd_spi_bus_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print,
        "<SPI_BUS %s, dc=%d, cs=%d, spi_mode=%d, pclk=%d, lcd_cmd_bits=%d, "
        "lcd_param_bits=%d, dc_low_on_data=%d, octal_mode=%d, lsb_first=%d, "
        "swap_color_bytes=%d>",
        self->name, self->dc_gpio_num, self->cs_gpio_num, self->spi_mode,
        self->pclk_hz, self->lcd_cmd_bits, self->lcd_param_bits,
        self->flags.dc_low_on_data, self->flags.octal_mode,
        self->flags.lsb_first, self->flags.swap_color_bytes);
}

static mp_obj_t esp_lcd_spi_bus_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args)
{
    enum {
        ARG_esp_spi_bus, ARG_spi_host, ARG_dc, ARG_cs, ARG_spi_mode,
        ARG_pclk_hz, ARG_lcd_cmd_bits, ARG_lcd_param_bits,
        ARG_dc_low_on_data, ARG_octal_mode, ARG_lsb_first, ARG_swap_color_bytes,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_bus,              MP_ARG_OBJ  | MP_ARG_REQUIRED                      },
        { MP_QSTR_spi_host,         MP_ARG_INT  | MP_ARG_REQUIRED                      },
        { MP_QSTR_dc,               MP_ARG_INT  | MP_ARG_REQUIRED                      },
        { MP_QSTR_cs,               MP_ARG_INT,                    {.u_int = -1       } },
        { MP_QSTR_spi_mode,         MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_int = 0        } },
        { MP_QSTR_pclk,             MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_int = 40000000 } },
        { MP_QSTR_lcd_cmd_bits,     MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_int = 8        } },
        { MP_QSTR_lcd_param_bits,   MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_int = 8        } },
        { MP_QSTR_dc_low_on_data,   MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_int = 0        } },
        { MP_QSTR_octal_mode,       MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false   } },
        { MP_QSTR_lsb_first,        MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false   } },
        { MP_QSTR_swap_color_bytes, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false   } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_obj_t bus_obj = args[ARG_esp_spi_bus].u_obj;
    if (!mp_obj_is_type(bus_obj, &esp_spi_bus_type))
        mp_raise_TypeError(MP_ERROR_TEXT("bus must be an esp_spi.SPIBus object"));

    esp_spi_bus_obj_t *spi_bus = MP_OBJ_TO_PTR(bus_obj);
    if (!spi_bus->initialized)
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("SPI bus not initialized"));

    esp_lcd_spi_bus_obj_t *self = mp_obj_malloc(esp_lcd_spi_bus_obj_t, type);
    self->base.type              = &esp_lcd_spi_bus_type;
    self->name                   = "esp_lcd_spi";
    self->bus_obj                = bus_obj;
    self->spi_host               = args[ARG_spi_host].u_int;
    self->dc_gpio_num            = args[ARG_dc].u_int;
    self->cs_gpio_num            = args[ARG_cs].u_int;
    self->spi_mode               = args[ARG_spi_mode].u_int;
    self->pclk_hz                = args[ARG_pclk_hz].u_int;
    self->lcd_cmd_bits           = args[ARG_lcd_cmd_bits].u_int;
    self->lcd_param_bits         = args[ARG_lcd_param_bits].u_int;
    self->flags.dc_low_on_data   = args[ARG_dc_low_on_data].u_int;
    self->flags.octal_mode       = args[ARG_octal_mode].u_bool;
    self->flags.lsb_first        = args[ARG_lsb_first].u_bool;
    self->flags.swap_color_bytes = args[ARG_swap_color_bytes].u_bool;

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num         = self->dc_gpio_num,
        .cs_gpio_num         = self->cs_gpio_num,
        .pclk_hz             = self->pclk_hz,
        .spi_mode            = self->spi_mode,
        .trans_queue_depth   = 30,
        .lcd_cmd_bits        = self->lcd_cmd_bits,
        .lcd_param_bits      = self->lcd_param_bits,
        .on_color_trans_done = lcd_panel_done,
        .flags = {
            .dc_low_on_data = self->flags.dc_low_on_data,
            .octal_mode     = self->flags.octal_mode,
            .lsb_first      = self->flags.lsb_first,
        },
    };
    esp_err_t ret = esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)(self->spi_host), &io_config, &self->io_handle);
    if (ret != ESP_OK)
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to create LCD panel IO"));

    self->spi_dev = NULL;
    return MP_OBJ_FROM_PTR(self);
}

static const mp_rom_map_elem_t esp_lcd_spi_bus_locals_dict_table[] = {};
static MP_DEFINE_CONST_DICT(esp_lcd_spi_bus_locals_dict, esp_lcd_spi_bus_locals_dict_table);

#if MICROPY_OBJ_TYPE_REPR == MICROPY_OBJ_TYPE_REPR_SLOT_INDEX
MP_DEFINE_CONST_OBJ_TYPE(
    esp_lcd_spi_bus_type, MP_QSTR_SPI_BUS, MP_TYPE_FLAG_NONE,
    print, esp_lcd_spi_bus_print,
    make_new, esp_lcd_spi_bus_make_new,
    locals_dict, &esp_lcd_spi_bus_locals_dict);
#else
const mp_obj_type_t esp_lcd_spi_bus_type = {
    { &mp_type_type },
    .name        = MP_QSTR_SPI_BUS,
    .print       = esp_lcd_spi_bus_print,
    .make_new    = esp_lcd_spi_bus_make_new,
    .locals_dict = (mp_obj_dict_t *)&esp_lcd_spi_bus_locals_dict,
};
#endif

// ═════════════════════════════════════════════════════════════════════════════
// ── ESPLCD ───────────────────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

// ── Rotation tables ───────────────────────────────────────────────────────────

typedef struct {
    uint16_t width, height, x_gap, y_gap;
    bool swap_xy, mirror_x, mirror_y;
} anim_rotation_t;

static const anim_rotation_t ROTATIONS_240x240[4] = {
    {240, 240, 0,  0,  false, false, false},
    {240, 240, 0,  0,  true,  true,  false},
    {240, 240, 0,  80, false, true,  true },
    {240, 240, 80, 0,  true,  false, true },
};
static const anim_rotation_t ROTATIONS_240x320[4] = {
    {240, 320, 0, 0, false, false, false},
    {320, 240, 0, 0, true,  true,  false},
    {240, 320, 0, 0, false, true,  true },
    {320, 240, 0, 0, true,  false, true },
};
static const anim_rotation_t ROTATIONS_170x320[4] = {
    {170, 320, 35, 0,  false, false, false},
    {320, 170, 0,  35, true,  true,  false},
    {170, 320, 35, 0,  false, true,  true },
    {320, 170, 0,  35, true,  false, true },
};

static const anim_rotation_t *get_rotation_table(uint16_t w, uint16_t h) {
    if (w == 240 && h == 240) return ROTATIONS_240x240;
    if (w == 240 && h == 320) return ROTATIONS_240x320;
    if (w == 170 && h == 320) return ROTATIONS_170x320;
    return ROTATIONS_240x240;
}

static void apply_rotation(anim_display_obj_t *self) {
    uint16_t base_w = (self->rotation % 2 == 0) ? self->width  : self->height;
    uint16_t base_h = (self->rotation % 2 == 0) ? self->height : self->width;
    const anim_rotation_t *table = get_rotation_table(base_w, base_h);
    const anim_rotation_t *r     = &table[self->rotation % 4];
    esp_lcd_panel_swap_xy(self->panel_handle, r->swap_xy);
    esp_lcd_panel_mirror(self->panel_handle, r->mirror_x, r->mirror_y);
    esp_lcd_panel_set_gap(self->panel_handle, r->x_gap, r->y_gap);
    self->width  = r->width;
    self->height = r->height;
}

// ── make_new ──────────────────────────────────────────────────────────────────

static mp_obj_t anim_display_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args)
{
    enum {
        ARG_bus, ARG_width, ARG_height, ARG_reset,
        ARG_rotation, ARG_inversion_mode, ARG_dma_rows, ARG_color_space,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_bus,            MP_ARG_OBJ  | MP_ARG_REQUIRED                    },
        { MP_QSTR_width,          MP_ARG_INT  | MP_ARG_REQUIRED                    },
        { MP_QSTR_height,         MP_ARG_INT  | MP_ARG_REQUIRED                    },
        { MP_QSTR_reset,          MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_int  = -1    } },
        { MP_QSTR_rotation,       MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_int  = 0     } },
        { MP_QSTR_inversion_mode, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = true  } },
        { MP_QSTR_dma_rows,       MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_int  = 16    } },
        { MP_QSTR_color_space,    MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_int  = 0     } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (!mp_obj_is_type(args[ARG_bus].u_obj, &esp_lcd_spi_bus_type))
        mp_raise_TypeError(MP_ERROR_TEXT("bus must be an esp_lcd.SPI_BUS object"));

    anim_display_obj_t *self   = m_new_obj(anim_display_obj_t);
    self->base.type            = &anim_display_type;
    self->bus                  = args[ARG_bus].u_obj;
    self->width                = args[ARG_width].u_int;
    self->height               = args[ARG_height].u_int;
    self->rst                  = (gpio_num_t)args[ARG_reset].u_int;
    self->rotation             = args[ARG_rotation].u_int % 4;
    self->inversion_mode       = args[ARG_inversion_mode].u_bool;
    self->dma_rows             = args[ARG_dma_rows].u_int;
    self->color_space          = args[ARG_color_space].u_int;
    self->panel_handle         = NULL;
    self->io_handle            = NULL;
    self->dma_buffer           = NULL;
    self->dma_buffer_size      = 0;

    esp_lcd_spi_bus_obj_t *bus = MP_OBJ_TO_PTR(self->bus);
    self->swap_color_bytes     = bus->flags.swap_color_bytes;

    return MP_OBJ_FROM_PTR(self);
}

// ── init ──────────────────────────────────────────────────────────────────────

static mp_obj_t anim_display_init(mp_obj_t self_in) {
    anim_display_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (self->panel_handle) {
        esp_lcd_panel_del(self->panel_handle);
        self->panel_handle = NULL;
    }

    esp_lcd_spi_bus_obj_t *bus = MP_OBJ_TO_PTR(self->bus);
    self->io_handle = bus->io_handle;

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = self->rst,
        .color_space    = self->color_space,
        .bits_per_pixel = 16,
    };
    esp_err_t ret = esp_lcd_new_panel_st7789(
        self->io_handle, &panel_config, &self->panel_handle);
    if (ret != ESP_OK)
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to create LCD panel"));

    esp_lcd_panel_reset(self->panel_handle);
    esp_lcd_panel_init(self->panel_handle);
    esp_lcd_panel_disp_on_off(self->panel_handle, true);
    esp_lcd_panel_invert_color(self->panel_handle, self->inversion_mode);
    apply_rotation(self);

    if (self->dma_buffer) {
        heap_caps_free(self->dma_buffer);
        self->dma_buffer = NULL;
    }
    if (self->dma_rows == 0) self->dma_rows = 16;
    self->dma_buffer_size = ((self->width * self->dma_rows * sizeof(uint16_t)) + 3) & ~3;
    self->dma_buffer = heap_caps_malloc(self->dma_buffer_size, MALLOC_CAP_DMA);
    if (!self->dma_buffer)
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("Failed to allocate DMA buffer"));

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(anim_display_init_obj, anim_display_init);

// ── deinit ────────────────────────────────────────────────────────────────────

static mp_obj_t anim_display_deinit(mp_obj_t self_in) {
    anim_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->panel_handle) {
        esp_lcd_panel_del(self->panel_handle);
        self->panel_handle = NULL;
    }
    if (self->dma_buffer) {
        heap_caps_free(self->dma_buffer);
        self->dma_buffer  = NULL;
        self->dma_buffer_size = 0;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(anim_display_deinit_obj, anim_display_deinit);

// ── rotation ──────────────────────────────────────────────────────────────────

static mp_obj_t anim_display_rotation(mp_obj_t self_in, mp_obj_t val) {
    anim_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->rotation = mp_obj_get_int(val) % 4;
    apply_rotation(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(anim_display_rotation_obj, anim_display_rotation);

// ── inversion_mode ────────────────────────────────────────────────────────────

static mp_obj_t anim_display_inversion_mode(mp_obj_t self_in, mp_obj_t val) {
    anim_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->inversion_mode = mp_obj_is_true(val);
    esp_lcd_panel_invert_color(self->panel_handle, self->inversion_mode);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(anim_display_inversion_mode_obj, anim_display_inversion_mode);

// ── blit_buffer ───────────────────────────────────────────────────────────────
// blit_buffer(buf, x, y, w, h)

static mp_obj_t anim_display_blit_buffer(size_t n_args, const mp_obj_t *args) {
    anim_display_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (!self->panel_handle || !self->dma_buffer)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Display not initialized"));

    mp_buffer_info_t buf_info;
    mp_get_buffer_raise(args[1], &buf_info, MP_BUFFER_READ);
    mp_int_t x = mp_obj_get_int(args[2]);
    mp_int_t y = mp_obj_get_int(args[3]);
    mp_int_t w = mp_obj_get_int(args[4]);
    mp_int_t h = mp_obj_get_int(args[5]);

    if (w <= 0 || h <= 0) return mp_const_none;
    if (buf_info.len < (size_t)(w * h * 2))
        mp_raise_ValueError(MP_ERROR_TEXT("Buffer too small"));

    // Clip to display bounds
    mp_int_t src_x  = (x < 0) ? -x : 0;
    mp_int_t src_y  = (y < 0) ? -y : 0;
    mp_int_t dst_x  = (x < 0) ?  0 : x;
    mp_int_t dst_y  = (y < 0) ?  0 : y;
    mp_int_t blit_w = w - src_x;
    mp_int_t blit_h = h - src_y;
    if (dst_x + blit_w > self->width)  blit_w = self->width  - dst_x;
    if (dst_y + blit_h > self->height) blit_h = self->height - dst_y;
    if (blit_w <= 0 || blit_h <= 0) return mp_const_none;

    uint16_t *src = (uint16_t *)buf_info.buf + src_y * w + src_x;

    for (mp_int_t row = 0; row < blit_h; row += self->dma_rows) {
        mp_int_t chunk = blit_h - row;
        if (chunk > self->dma_rows) chunk = self->dma_rows;

        for (mp_int_t r = 0; r < chunk; r++) {
            uint16_t *s = src + (row + r) * w;
            uint16_t *d = self->dma_buffer + r * blit_w;
            if (self->swap_color_bytes) {
                for (mp_int_t c = 0; c < blit_w; c++)
                    d[c] = ((s[c] >> 8) | (s[c] << 8)) & 0xFFFF;
            } else {
                memcpy(d, s, blit_w * sizeof(uint16_t));
            }
        }

        lcd_panel_active = true;
        esp_lcd_panel_draw_bitmap(self->panel_handle,
            dst_x, dst_y + row, dst_x + blit_w, dst_y + row + chunk,
            self->dma_buffer);

        for (int i = 0; i < 100 && lcd_panel_active; i++) {}
        int timeout = 1000;
        while (lcd_panel_active && timeout-- > 0)
            vTaskDelay(1 / portTICK_PERIOD_MS);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(anim_display_blit_buffer_obj, 6, 6, anim_display_blit_buffer);

// ── png_write ─────────────────────────────────────────────────────────────────
// tft.png_write(display_buf, filename {, x, y, w, h})

static int32_t _png_write_cb(PNGFILE *f, uint8_t *buf, int32_t len) {
    return mp_write((mp_file_t *)f->fHandle, buf, len);
}
static int32_t _png_read_cb(PNGFILE *f, uint8_t *buf, int32_t len) {
    return mp_readinto((mp_file_t *)f->fHandle, buf, len);
}
static int32_t _png_seek_cb(PNGFILE *f, int32_t pos) {
    return mp_seek((mp_file_t *)f->fHandle, pos, SEEK_SET);
}
static void *_png_open_cb(const char *name) {
    return (void *)mp_open(name, "w+b");
}
static void _png_close_cb(PNGFILE *f) {
    mp_close((mp_file_t *)f->fHandle);
}

static mp_obj_t anim_display_png_write(size_t n_args, const mp_obj_t *args) {
    anim_display_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_buffer_info_t buf_info;
    mp_get_buffer_raise(args[1], &buf_info, MP_BUFFER_READ);
    const char *filename = mp_obj_str_get_str(args[2]);

    mp_int_t x = 0, y = 0;
    mp_int_t w = self->width, h = self->height;
    if (n_args == 7) {
        x = mp_obj_get_int(args[3]);
        y = mp_obj_get_int(args[4]);
        w = mp_obj_get_int(args[5]);
        h = mp_obj_get_int(args[6]);
    } else if (n_args != 3) {
        mp_raise_TypeError(MP_ERROR_TEXT("png_write: expected 2 or 6 args after self"));
    }
    if (x < 0 || y < 0 || x + w > self->width || y + h > self->height)
        mp_raise_ValueError(MP_ERROR_TEXT("Crop region out of bounds"));

    uint8_t *work_buf = m_malloc(w * 3 * 2);
    if (!work_buf)
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("png_write: out of memory"));

    PNGIMAGE *png = m_malloc(sizeof(PNGIMAGE));
    if (!png) {
        m_free(work_buf);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("png_write: out of memory"));
    }

    int rc = PNG_openFile(png, filename, _png_open_cb, _png_close_cb,
                          _png_read_cb, _png_write_cb, _png_seek_cb);
    if (rc != PNG_SUCCESS) {
        m_free(work_buf);
        m_free(png);
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("png_write: failed to open file"));
    }

    rc = PNG_encodeBegin(png, w, h, PNG_PIXEL_TRUECOLOR, 24, NULL, 9);
    if (rc == PNG_SUCCESS) {
        uint16_t *src = (uint16_t *)buf_info.buf + y * self->width + x;
        for (int row = 0; row < h && rc == PNG_SUCCESS; row++) {
            rc = PNG_addRGB565Line(png, src, (uint16_t *)work_buf, row);
            src += self->width;
        }
        PNG_close(png);
    }

    m_free(work_buf);
    m_free(png);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(anim_display_png_write_obj, 3, 7, anim_display_png_write);

// ── ESPLCD locals dict ────────────────────────────────────────────────────────

static const mp_rom_map_elem_t anim_display_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init),           MP_ROM_PTR(&anim_display_init_obj)           },
    { MP_ROM_QSTR(MP_QSTR_deinit),         MP_ROM_PTR(&anim_display_deinit_obj)         },
    { MP_ROM_QSTR(MP_QSTR_rotation),       MP_ROM_PTR(&anim_display_rotation_obj)       },
    { MP_ROM_QSTR(MP_QSTR_inversion_mode), MP_ROM_PTR(&anim_display_inversion_mode_obj) },
    { MP_ROM_QSTR(MP_QSTR_blit_buffer),    MP_ROM_PTR(&anim_display_blit_buffer_obj)    },
    { MP_ROM_QSTR(MP_QSTR_png_write),      MP_ROM_PTR(&anim_display_png_write_obj)      },
};
static MP_DEFINE_CONST_DICT(anim_display_locals_dict, anim_display_locals_dict_table);

#if MICROPY_OBJ_TYPE_REPR == MICROPY_OBJ_TYPE_REPR_SLOT_INDEX
MP_DEFINE_CONST_OBJ_TYPE(
    anim_display_type, MP_QSTR_ESPLCD, MP_TYPE_FLAG_NONE,
    make_new, anim_display_make_new,
    locals_dict, &anim_display_locals_dict);
#else
const mp_obj_type_t anim_display_type = {
    { &mp_type_type },
    .name        = MP_QSTR_ESPLCD,
    .make_new    = anim_display_make_new,
    .locals_dict = (mp_obj_dict_t *)&anim_display_locals_dict,
};
#endif

// ═════════════════════════════════════════════════════════════════════════════
// ── Module table ─────────────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

static const mp_rom_map_elem_t esp_lcd_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_esp_lcd) },
    { MP_ROM_QSTR(MP_QSTR_SPI_BUS),  MP_ROM_PTR(&esp_lcd_spi_bus_type) },
    { MP_ROM_QSTR(MP_QSTR_ESPLCD),   MP_ROM_PTR(&anim_display_type)    },
};
static MP_DEFINE_CONST_DICT(esp_lcd_module_globals, esp_lcd_module_globals_table);

const mp_obj_module_t mp_module_esp_lcd = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&esp_lcd_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_esp_lcd, mp_module_esp_lcd);

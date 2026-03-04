/*
 * animation.c — Sprite compositing, display management, and drawing
 * for MicroPython on ESP32-S3.
 *
 * Replaces s3lcd.c with a single-buffer pipeline:
 *   fill_background → draw_all → blit_buffer
 *
 * No internal framebuffer — all drawing targets a Python bytearray
 * (display_buf) that you manage and send to the display yourself.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "py/obj.h"
#include "py/runtime.h"
#include "py/mpconfig.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_commands.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd.h"
#include "mpfile.h"
#include "pngenc/pngenc.h"

// Fix for MicroPython > 1.21
#if MICROPY_VERSION_MAJOR >= 1 && MICROPY_VERSION_MINOR > 21
#include "extmod/modmachine.h"
#else
#include "extmod/machine_spi.h"
#endif

// ─── Constants ────────────────────────────────────────────────────────────────

#define MAX_SLOTS       16
#define MAGIC_COLOR     58572   // RGB565 transparency key: RGB(231,154,99)

// ST77XX commands needed for rotation/inversion
#define ST77XX_MADCTL   0x36
#define ST77XX_INVOFF   0x20
#define ST77XX_INVON    0x21
#define MADCTL_MY       0x80
#define MADCTL_MX       0x40
#define MADCTL_MV       0x20
#define MADCTL_RGB      0x00
#define MADCTL_BGR      0x08

// ─── Display object ───────────────────────────────────────────────────────────

typedef struct _anim_display_obj_t {
    mp_obj_base_t              base;
    mp_obj_t                   bus;
    esp_lcd_panel_io_handle_t  io_handle;
    esp_lcd_panel_handle_t     panel_handle;
    uint16_t                   width;
    uint16_t                   height;
    uint8_t                    rotation;
    bool                       inversion_mode;
    bool                       swap_color_bytes;
    gpio_num_t                 rst;
    uint8_t                    color_space;
    uint16_t                   dma_rows;
    uint16_t                  *dma_buffer;
    size_t                     dma_buffer_size;
} anim_display_obj_t;

extern const mp_obj_type_t anim_display_type;

// DMA completion flag — shared with esp_lcd.c via extern
extern volatile bool lcd_panel_active;

// ─── Rotation tables ─────────────────────────────────────────────────────────

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
    return ROTATIONS_240x240; // default
}

static void apply_rotation(anim_display_obj_t *self) {
    const anim_rotation_t *table = get_rotation_table(
        self->rotation % 2 == 0 ? self->width : self->height,
        self->rotation % 2 == 0 ? self->height : self->width
    );
    const anim_rotation_t *r = &table[self->rotation % 4];
    esp_lcd_panel_swap_xy(self->panel_handle, r->swap_xy);
    esp_lcd_panel_mirror(self->panel_handle, r->mirror_x, r->mirror_y);
    esp_lcd_panel_set_gap(self->panel_handle, r->x_gap, r->y_gap);
    self->width  = r->width;
    self->height = r->height;
}

// ─── ESPLCD.make_new ─────────────────────────────────────────────────────────

static mp_obj_t anim_display_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args)
{
    enum {
        ARG_bus, ARG_width, ARG_height, ARG_reset,
        ARG_rotation, ARG_inversion_mode, ARG_dma_rows, ARG_color_space,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_bus,             MP_ARG_OBJ  | MP_ARG_REQUIRED                       },
        { MP_QSTR_width,           MP_ARG_INT  | MP_ARG_REQUIRED                       },
        { MP_QSTR_height,          MP_ARG_INT  | MP_ARG_REQUIRED                       },
        { MP_QSTR_reset,           MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_int  = -1       } },
        { MP_QSTR_rotation,        MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_int  = 0        } },
        { MP_QSTR_inversion_mode,  MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = true     } },
        { MP_QSTR_dma_rows,        MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_int  = 16       } },
        { MP_QSTR_color_space,     MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_int  = 0        } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (!mp_obj_is_type(args[ARG_bus].u_obj, &esp_lcd_spi_bus_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("bus must be an esp_lcd.SPI_BUS object"));
    }

    anim_display_obj_t *self = m_new_obj(anim_display_obj_t);
    self->base.type        = &anim_display_type;
    self->bus              = args[ARG_bus].u_obj;
    self->width            = args[ARG_width].u_int;
    self->height           = args[ARG_height].u_int;
    self->rst              = (gpio_num_t)args[ARG_reset].u_int;
    self->rotation         = args[ARG_rotation].u_int % 4;
    self->inversion_mode   = args[ARG_inversion_mode].u_bool;
    self->dma_rows         = args[ARG_dma_rows].u_int;
    self->color_space      = args[ARG_color_space].u_int;
    self->panel_handle     = NULL;
    self->io_handle        = NULL;
    self->dma_buffer       = NULL;
    self->dma_buffer_size  = 0;

    esp_lcd_spi_bus_obj_t *bus = MP_OBJ_TO_PTR(self->bus);
    self->swap_color_bytes = bus->flags.swap_color_bytes;

    return MP_OBJ_FROM_PTR(self);
}

// ─── ESPLCD.init ─────────────────────────────────────────────────────────────

static mp_obj_t anim_display_init(mp_obj_t self_in) {
    anim_display_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (self->panel_handle != NULL) {
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
    esp_err_t ret = esp_lcd_new_panel_st7789(self->io_handle, &panel_config, &self->panel_handle);
    if (ret != ESP_OK) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to create LCD panel"));
    }

    esp_lcd_panel_reset(self->panel_handle);
    esp_lcd_panel_init(self->panel_handle);
    esp_lcd_panel_disp_on_off(self->panel_handle, true);
    esp_lcd_panel_invert_color(self->panel_handle, self->inversion_mode);
    apply_rotation(self);

    // Allocate DMA buffer — no internal framebuffer
    if (self->dma_buffer != NULL) {
        heap_caps_free(self->dma_buffer);
        self->dma_buffer = NULL;
    }
    if (self->dma_rows == 0) self->dma_rows = 16;
    self->dma_buffer_size = ((self->width * self->dma_rows * sizeof(uint16_t)) + 3) & ~3;
    self->dma_buffer = heap_caps_malloc(self->dma_buffer_size, MALLOC_CAP_DMA);
    if (self->dma_buffer == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("Failed to allocate DMA buffer"));
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(anim_display_init_obj, anim_display_init);

// ─── ESPLCD.deinit ───────────────────────────────────────────────────────────

static mp_obj_t anim_display_deinit(mp_obj_t self_in) {
    anim_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->panel_handle) {
        esp_lcd_panel_del(self->panel_handle);
        self->panel_handle = NULL;
    }
    if (self->dma_buffer) {
        heap_caps_free(self->dma_buffer);
        self->dma_buffer = NULL;
        self->dma_buffer_size = 0;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(anim_display_deinit_obj, anim_display_deinit);

// ─── ESPLCD.rotation ─────────────────────────────────────────────────────────

static mp_obj_t anim_display_rotation(mp_obj_t self_in, mp_obj_t val) {
    anim_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->rotation = mp_obj_get_int(val) % 4;
    apply_rotation(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(anim_display_rotation_obj, anim_display_rotation);

// ─── ESPLCD.inversion_mode ───────────────────────────────────────────────────

static mp_obj_t anim_display_inversion_mode(mp_obj_t self_in, mp_obj_t val) {
    anim_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->inversion_mode = mp_obj_is_true(val);
    esp_lcd_panel_invert_color(self->panel_handle, self->inversion_mode);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(anim_display_inversion_mode_obj, anim_display_inversion_mode);

// ─── ESPLCD.blit_buffer ──────────────────────────────────────────────────────
// Send an external buffer directly to the display via DMA.
// blit_buffer(buf, x, y, w, h)

static void _dma_send(anim_display_obj_t *self, uint16_t *src,
                       uint16_t row, uint16_t rows, size_t len) {
    if (self->swap_color_bytes) {
        for (size_t i = 0; i < len; i++) {
            self->dma_buffer[i] = ((src[i] >> 8) | (src[i] << 8)) & 0xFFFF;
        }
    } else {
        memcpy(self->dma_buffer, src, len * sizeof(uint16_t));
    }
    lcd_panel_active = true;
    esp_err_t ret = esp_lcd_panel_draw_bitmap(self->panel_handle,
        0, row, self->width, row + rows, self->dma_buffer);
    if (ret != ESP_OK) {
        lcd_panel_active = false;
        return;
    }
    for (int i = 0; i < 100 && lcd_panel_active; i++) {}
    int timeout = 1000;
    while (lcd_panel_active && timeout-- > 0) {
        vTaskDelay(1 / portTICK_PERIOD_MS);
    }
}

static mp_obj_t anim_display_blit_buffer(size_t n_args, const mp_obj_t *args) {
    anim_display_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (!self->panel_handle || !self->dma_buffer) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Display not initialized"));
    }

    mp_buffer_info_t buf_info;
    mp_get_buffer_raise(args[1], &buf_info, MP_BUFFER_READ);

    mp_int_t x = mp_obj_get_int(args[2]);
    mp_int_t y = mp_obj_get_int(args[3]);
    mp_int_t w = mp_obj_get_int(args[4]);
    mp_int_t h = mp_obj_get_int(args[5]);

    if (w <= 0 || h <= 0) return mp_const_none;
    if (buf_info.len < (size_t)(w * h * 2)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Buffer too small"));
    }

    // Clip to display bounds
    mp_int_t src_x  = (x < 0) ? -x : 0;
    mp_int_t src_y  = (y < 0) ? -y : 0;
    mp_int_t dst_x  = (x < 0) ? 0 : x;
    mp_int_t dst_y  = (y < 0) ? 0 : y;
    mp_int_t blit_w = w - src_x;
    mp_int_t blit_h = h - src_y;
    if (dst_x + blit_w > self->width)  blit_w = self->width  - dst_x;
    if (dst_y + blit_h > self->height) blit_h = self->height - dst_y;
    if (blit_w <= 0 || blit_h <= 0) return mp_const_none;

    uint16_t *src = (uint16_t *)buf_info.buf + src_y * w + src_x;
    size_t pixels_per_dma = self->dma_rows * self->width;

    // Send row by row in DMA-sized chunks
    for (mp_int_t row = 0; row < blit_h; row += self->dma_rows) {
        mp_int_t chunk = blit_h - row;
        if (chunk > self->dma_rows) chunk = self->dma_rows;
        size_t pixels = chunk * blit_w;

        // Copy this chunk into a contiguous temp area in dma_buffer
        for (mp_int_t r = 0; r < chunk; r++) {
            uint16_t *s = src + (row + r) * w;
            uint16_t *d = self->dma_buffer + r * blit_w;
            if (self->swap_color_bytes) {
                for (mp_int_t c = 0; c < blit_w; c++) {
                    d[c] = ((s[c] >> 8) | (s[c] << 8)) & 0xFFFF;
                }
            } else {
                memcpy(d, s, blit_w * sizeof(uint16_t));
            }
        }

        lcd_panel_active = true;
        esp_lcd_panel_draw_bitmap(self->panel_handle,
            dst_x, dst_y + row,
            dst_x + blit_w, dst_y + row + chunk,
            self->dma_buffer);

        for (int i = 0; i < 100 && lcd_panel_active; i++) {}
        int timeout = 1000;
        while (lcd_panel_active && timeout-- > 0) {
            vTaskDelay(1 / portTICK_PERIOD_MS);
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(anim_display_blit_buffer_obj, 6, 6, anim_display_blit_buffer);

// ─── ESPLCD.png_write ────────────────────────────────────────────────────────
// png_write(display_buf, filename {, x, y, width, height})
// Saves display_buf (or a crop of it) as a PNG file via MicroPython VFS.

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
    // png_write(self, display_buf, filename {, x, y, w, h})
    mp_buffer_info_t buf_info;
    mp_get_buffer_raise(args[1], &buf_info, MP_BUFFER_READ);
    const char *filename = mp_obj_str_get_str(args[2]);

    anim_display_obj_t *self = MP_OBJ_TO_PTR(args[0]);

    mp_int_t x = 0, y = 0;
    mp_int_t w = self->width, h = self->height;

    if (n_args == 7) {
        x = mp_obj_get_int(args[3]);
        y = mp_obj_get_int(args[4]);
        w = mp_obj_get_int(args[5]);
        h = mp_obj_get_int(args[6]);
    } else if (n_args != 3) {
        mp_raise_TypeError(MP_ERROR_TEXT("png_write requires 2 or 6 arguments after self"));
    }

    if (x < 0 || y < 0 || x + w > self->width || y + h > self->height) {
        mp_raise_ValueError(MP_ERROR_TEXT("Crop region out of bounds"));
    }

    int work_buf_size = w * 3 * 2;
    uint16_t *work_buf = m_malloc(work_buf_size);
    if (!work_buf) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("png_write: out of memory"));
    }

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
            rc = PNG_addRGB565Line(png, src, work_buf, row);
            src += self->width;
        }
        PNG_close(png);
    }

    m_free(work_buf);
    m_free(png);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(anim_display_png_write_obj, 3, 7, anim_display_png_write);

// ─── ESPLCD locals dict ───────────────────────────────────────────────────────

static const mp_rom_map_elem_t anim_display_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init),            MP_ROM_PTR(&anim_display_init_obj)            },
    { MP_ROM_QSTR(MP_QSTR_deinit),          MP_ROM_PTR(&anim_display_deinit_obj)          },
    { MP_ROM_QSTR(MP_QSTR_rotation),        MP_ROM_PTR(&anim_display_rotation_obj)        },
    { MP_ROM_QSTR(MP_QSTR_inversion_mode),  MP_ROM_PTR(&anim_display_inversion_mode_obj)  },
    { MP_ROM_QSTR(MP_QSTR_blit_buffer),     MP_ROM_PTR(&anim_display_blit_buffer_obj)     },
    { MP_ROM_QSTR(MP_QSTR_png_write),       MP_ROM_PTR(&anim_display_png_write_obj)       },
};
static MP_DEFINE_CONST_DICT(anim_display_locals_dict, anim_display_locals_dict_table);

#if MICROPY_OBJ_TYPE_REPR == MICROPY_OBJ_TYPE_REPR_SLOT_INDEX
MP_DEFINE_CONST_OBJ_TYPE(
    anim_display_type,
    MP_QSTR_ESPLCD,
    MP_TYPE_FLAG_NONE,
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

// ═════════════

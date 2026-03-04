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

// ═══════════════════════════════════════════════════════════════════════════════
// ─── Slot system ──────────────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

typedef struct {
    uint8_t  *buf;
    int16_t   x, y, w, h;
    bool      enabled;
    // Vertical clip
    int16_t   clip_y;
    bool      clip_y_enabled;
    bool      clip_y_after;   // true = hide y >= clip_y, false = hide y < clip_y
    // Horizontal clip
    int16_t   clip_x;
    bool      clip_x_enabled;
    bool      clip_x_after;   // true = hide x >= clip_x, false = hide x < clip_x
} sprite_slot_t;

static sprite_slot_t slots[MAX_SLOTS];
static int16_t display_w = 240;
static int16_t display_h = 240;

// ─── set_display_size ────────────────────────────────────────────────────────

static mp_obj_t animation_set_display_size(mp_obj_t w_in, mp_obj_t h_in) {
    display_w = (int16_t)mp_obj_get_int(w_in);
    display_h = (int16_t)mp_obj_get_int(h_in);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(animation_set_display_size_obj, animation_set_display_size);

// ─── clear_slots ─────────────────────────────────────────────────────────────

static mp_obj_t animation_clear_slots(void) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        slots[i].enabled        = false;
        slots[i].buf            = NULL;
        slots[i].clip_y_enabled = false;
        slots[i].clip_x_enabled = false;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(animation_clear_slots_obj, animation_clear_slots);

// ─── set_slot ────────────────────────────────────────────────────────────────

static mp_obj_t animation_set_slot(size_t n_args, const mp_obj_t *args) {
    int idx = mp_obj_get_int(args[0]);
    if (idx < 0 || idx >= MAX_SLOTS)
        mp_raise_ValueError(MP_ERROR_TEXT("slot index out of range"));
    mp_buffer_info_t info;
    mp_get_buffer_raise(args[1], &info, MP_BUFFER_READ);
    slots[idx].buf            = (uint8_t *)info.buf;
    slots[idx].x              = (int16_t)mp_obj_get_int(args[2]);
    slots[idx].y              = (int16_t)mp_obj_get_int(args[3]);
    slots[idx].w              = (int16_t)mp_obj_get_int(args[4]);
    slots[idx].h              = (int16_t)mp_obj_get_int(args[5]);
    slots[idx].enabled        = true;
    slots[idx].clip_y_enabled = false;
    slots[idx].clip_x_enabled = false;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_set_slot_obj, 6, 6, animation_set_slot);

// ─── update_slot ─────────────────────────────────────────────────────────────

static mp_obj_t animation_update_slot(size_t n_args, const mp_obj_t *args) {
    int idx = mp_obj_get_int(args[0]);
    if (idx < 0 || idx >= MAX_SLOTS)
        mp_raise_ValueError(MP_ERROR_TEXT("slot index out of range"));
    mp_buffer_info_t info;
    mp_get_buffer_raise(args[1], &info, MP_BUFFER_READ);
    slots[idx].buf = (uint8_t *)info.buf;
    slots[idx].x   = (int16_t)mp_obj_get_int(args[2]);
    slots[idx].y   = (int16_t)mp_obj_get_int(args[3]);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_update_slot_obj, 4, 4, animation_update_slot);

// ─── update_slot_pos ─────────────────────────────────────────────────────────

static mp_obj_t animation_update_slot_pos(size_t n_args, const mp_obj_t *args) {
    int idx = mp_obj_get_int(args[0]);
    if (idx < 0 || idx >= MAX_SLOTS)
        mp_raise_ValueError(MP_ERROR_TEXT("slot index out of range"));
    slots[idx].x = (int16_t)mp_obj_get_int(args[1]);
    slots[idx].y = (int16_t)mp_obj_get_int(args[2]);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_update_slot_pos_obj, 3, 3, animation_update_slot_pos);

// ─── update_slot_buf ─────────────────────────────────────────────────────────

static mp_obj_t animation_update_slot_buf(size_t n_args, const mp_obj_t *args) {
    int idx = mp_obj_get_int(args[0]);
    if (idx < 0 || idx >= MAX_SLOTS)
        mp_raise_ValueError(MP_ERROR_TEXT("slot index out of range"));
    mp_buffer_info_t info;
    mp_get_buffer_raise(args[1], &info, MP_BUFFER_READ);
    slots[idx].buf = (uint8_t *)info.buf;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_update_slot_buf_obj, 2, 2, animation_update_slot_buf);

// ─── enable_slot ─────────────────────────────────────────────────────────────

static mp_obj_t animation_enable_slot(size_t n_args, const mp_obj_t *args) {
    int idx = mp_obj_get_int(args[0]);
    if (idx < 0 || idx >= MAX_SLOTS)
        mp_raise_ValueError(MP_ERROR_TEXT("slot index out of range"));
    slots[idx].enabled = mp_obj_is_true(args[1]);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_enable_slot_obj, 2, 2, animation_enable_slot);

// ─── set_slot_clip ───────────────────────────────────────────────────────────
// set_slot_clip(index, clip_x, clip_x_dir, clip_y, clip_y_dir)
// clip_x/clip_y: pixel coordinate, 0 = disabled
// clip_x_dir/clip_y_dir: "before" or "after"
//   "after"  = hide pixels AT or AFTER the cutoff (>= cutoff)
//   "before" = hide pixels BEFORE the cutoff (< cutoff)

static mp_obj_t animation_set_slot_clip(size_t n_args, const mp_obj_t *args) {
    int idx = mp_obj_get_int(args[0]);
    if (idx < 0 || idx >= MAX_SLOTS)
        mp_raise_ValueError(MP_ERROR_TEXT("slot index out of range"));

    int16_t clip_x = (int16_t)mp_obj_get_int(args[1]);
    const char *cx_dir = mp_obj_str_get_str(args[2]);
    int16_t clip_y = (int16_t)mp_obj_get_int(args[3]);
    const char *cy_dir = mp_obj_str_get_str(args[4]);

    slots[idx].clip_x         = clip_x;
    slots[idx].clip_x_enabled = (clip_x != 0);
    slots[idx].clip_x_after   = (cx_dir[0] == 'a'); // "after" vs "before"

    slots[idx].clip_y         = clip_y;
    slots[idx].clip_y_enabled = (clip_y != 0);
    slots[idx].clip_y_after   = (cy_dir[0] == 'a');

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_set_slot_clip_obj, 5, 5, animation_set_slot_clip);

// ─── Internal blit ───────────────────────────────────────────────────────────

static void blit_slot(sprite_slot_t *slot, uint8_t *dst) {
    uint8_t *src = slot->buf;
    int16_t sw   = slot->w;
    int16_t sh   = slot->h;
    int16_t ox   = slot->x;
    int16_t oy   = slot->y;

    for (int row = 0; row < sh; row++) {
        int target_row = oy + row;
        if (target_row < 0 || target_row >= display_h) continue;

        // Vertical clip
        if (slot->clip_y_enabled) {
            if (slot->clip_y_after  && target_row >= slot->clip_y) continue;
            if (!slot->clip_y_after && target_row <  slot->clip_y) continue;
        }

        int src_row_base = row * sw * 2;
        int dst_row_base = target_row * display_w * 2;

        for (int col = 0; col < sw; col++) {
            int target_col = ox + col;
            if (target_col < 0 || target_col >= display_w) continue;

            // Horizontal clip
            if (slot->clip_x_enabled) {
                if (slot->clip_x_after  && target_col >= slot->clip_x) continue;
                if (!slot->clip_x_after && target_col <  slot->clip_x) continue;
            }

            int   si    = src_row_base + col * 2;
            int   color = (src[si] << 8) | src[si + 1];
            if (color != MAGIC_COLOR) {
                int di  = dst_row_base + target_col * 2;
                dst[di]     = src[si];
                dst[di + 1] = src[si + 1];
            }
        }
    }
}

// ─── draw_all ────────────────────────────────────────────────────────────────

static mp_obj_t animation_draw_all(mp_obj_t display_buf_in) {
    mp_buffer_info_t info;
    mp_get_buffer_raise(display_buf_in, &info, MP_BUFFER_WRITE);
    uint8_t *dst = (uint8_t *)info.buf;
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (!slots[i].enabled || slots[i].buf == NULL) continue;
        blit_slot(&slots[i], dst);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(animation_draw_all_obj, animation_draw_all);

// ─── fill_background ─────────────────────────────────────────────────────────

static mp_obj_t animation_fill_background(mp_obj_t dst_in, mp_obj_t src_in) {
    mp_buffer_info_t dst_info, src_info;
    mp_get_buffer_raise(dst_in, &dst_info, MP_BUFFER_WRITE);
    mp_get_buffer_raise(src_in, &src_info, MP_BUFFER_READ);
    size_t len = dst_info.len < src_info.len ? dst_info.len : src_info.len;
    memcpy(dst_info.buf, src_info.buf, len);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(animation_fill_background_obj, animation_fill_background);

// ─── flip_buf_horizontal ─────────────────────────────────────────────────────

static mp_obj_t animation_flip_buf_horizontal(size_t n_args, const mp_obj_t *args) {
    mp_buffer_info_t src_info, dst_info;
    mp_get_buffer_raise(args[0], &src_info, MP_BUFFER_READ);
    mp_get_buffer_raise(args[1], &dst_info, MP_BUFFER_WRITE);
    int w = mp_obj_get_int(args[2]);
    int h = mp_obj_get_int(args[3]);
    uint8_t *src = (uint8_t *)src_info.buf;
    uint8_t *dst = (uint8_t *)dst_info.buf;
    int stride = w * 2;
    for (int row = 0; row < h; row++) {
        int base = row * stride;
        for (int col = 0; col < w; col++) {
            int si = base + col * 2;
            int di = base + (w - 1 - col) * 2;
            dst[di]     = src[si];
            dst[di + 1] = src[si + 1];
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_flip_buf_horizontal_obj, 4, 4, animation_flip_buf_horizontal);

// ─── flip_buf_vertical ───────────────────────────────────────────────────────

static mp_obj_t animation_flip_buf_vertical(size_t n_args, const mp_obj_t *args) {
    mp_buffer_info_t src_info, dst_info;
    mp_get_buffer_raise(args[0], &src_info, MP_BUFFER_READ);
    mp_get_buffer_raise(args[1], &dst_info, MP_BUFFER_WRITE);
    int w = mp_obj_get_int(args[2]);
    int h = mp_obj_get_int(args[3]);
    uint8_t *src = (uint8_t *)src_info.buf;
    uint8_t *dst = (uint8_t *)dst_info.buf;
    int stride = w * 2;
    for (int row = 0; row < h; row++) {
        memcpy(dst + row * stride, src + (h - 1 - row) * stride, stride);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_flip_buf_vertical_obj, 4, 4, animation_flip_buf_vertical);

// ═══════════════════════════════════════════════════════════════════════════════
// ─── Drawing functions ────────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

// All drawing functions write into a Python bytearray (display_buf).
// Colors are RGB565 uint16 values. bg=-1 means transparent (skip bg pixels).

// ─── fill_rect ───────────────────────────────────────────────────────────────
// fill_rect(display_buf, x, y, w, h, color)

static mp_obj_t animation_fill_rect(size_t n_args, const mp_obj_t *args) {
    mp_buffer_info_t info;
    mp_get_buffer_raise(args[0], &info, MP_BUFFER_WRITE);
    uint8_t *buf = (uint8_t *)info.buf;

    int x     = mp_obj_get_int(args[1]);
    int y     = mp_obj_get_int(args[2]);
    int w     = mp_obj_get_int(args[3]);
    int h     = mp_obj_get_int(args[4]);
    int color = mp_obj_get_int(args[5]);

    uint8_t hi = (color >> 8) & 0xFF;
    uint8_t lo =  color       & 0xFF;

    // Clip to display bounds
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > display_w) w = display_w - x;
    if (y + h > display_h) h = display_h - y;
    if (w <= 0 || h <= 0) return mp_const_none;

    for (int row = 0; row < h; row++) {
        int base = (y + row) * display_w * 2 + x * 2;
        for (int col = 0; col < w; col++) {
            buf[base + col * 2]     = hi;
            buf[base + col * 2 + 1] = lo;
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_fill_rect_obj, 6, 6, animation_fill_rect);

// ─── scroll ──────────────────────────────────────────────────────────────────
// scroll(display_buf, dx, dy {, fill_color})
// dx > 0 = scroll right, dx < 0 = scroll left
// dy > 0 = scroll down,  dy < 0 = scroll up
// fill_color defaults to 0 (BLACK)

static mp_obj_t animation_scroll(size_t n_args, const mp_obj_t *args) {
    mp_buffer_info_t info;
    mp_get_buffer_raise(args[0], &info, MP_BUFFER_WRITE);
    uint16_t *buf = (uint16_t *)info.buf;

    int dx   = mp_obj_get_int(args[1]);
    int dy   = mp_obj_get_int(args[2]);
    int fill = 0;
    if (n_args > 3) fill = mp_obj_get_int(args[3]);

    int W = display_w;
    int H = display_h;

    // Determine iteration order to avoid overwriting src before reading
    int y_start, y_end, y_step;
    int x_start, x_end, x_step;

    if (dy >= 0) { y_start = H - 1; y_end = -1;  y_step = -1; }
    else         { y_start = 0;     y_end = H;    y_step =  1; }
    if (dx >= 0) { x_start = W - 1; x_end = -1;  x_step = -1; }
    else         { x_start = 0;     x_end = W;    x_step =  1; }

    for (int y = y_start; y != y_end; y += y_step) {
        for (int x = x_start; x != x_end; x += x_step) {
            int src_x = x - dx;
            int src_y = y - dy;
            if (src_x >= 0 && src_x < W && src_y >= 0 && src_y < H) {
                buf[y * W + x] = buf[src_y * W + src_x];
            } else {
                buf[y * W + x] = (uint16_t)fill;
            }
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_scroll_obj, 3, 4, animation_scroll);

// ─── write (proportional bitmap font) ────────────────────────────────────────
// write(font, text, x, y, fg, display_buf {, bg})
// bg defaults to -1 (transparent)

static uint32_t _bs_bit = 0;
static uint8_t *_bitmap_data = NULL;

static uint8_t _get_color(uint8_t bpp) {
    uint8_t color = 0;
    for (int i = 0; i < bpp; i++) {
        color <<= 1;
        color |= (_bitmap_data[_bs_bit / 8] & (1 << (7 - (_bs_bit % 8)))) > 0;
        _bs_bit++;
    }
    return color;
}

static mp_obj_t animation_write(size_t n_args, const mp_obj_t *args) {
    // write(font, text, x, y, fg, display_buf {, bg=-1})
    mp_obj_module_t *font = MP_OBJ_TO_PTR(args[0]);

    const char *text;
    if (mp_obj_is_int(args[1])) {
        // single character as int
        static char single[2] = {0, 0};
        single[0] = (char)(mp_obj_get_int(args[1]) & 0xFF);
        text = single;
    } else {
        text = mp_obj_str_get_str(args[1]);
    }

    int x  = mp_obj_get_int(args[2]);
    int y  = mp_obj_get_int(args[3]);
    int fg = mp_obj_get_int(args[4]);
    int bg = -1;
    if (n_args > 6) bg = mp_obj_get_int(args[6]);

    mp_buffer_info_t buf_info;
    mp_get_buffer_raise(args[5], &buf_info, MP_BUFFER_WRITE);
    uint16_t *dst = (uint16_t *)buf_info.buf;

    mp_obj_dict_t *dict = MP_OBJ_TO_PTR(font->globals);

    // Read font properties
    const uint8_t bpp = mp_obj_get_int(
        mp_obj_dict_get(dict, MP_OBJ_NEW_QSTR(MP_QSTR_BPP)));
    const uint8_t height = mp_obj_get_int(
        mp_obj_dict_get(dict, MP_OBJ_NEW_QSTR(MP_QSTR_HEIGHT)));
    const uint8_t offset_width = mp_obj_get_int(
        mp_obj_dict_get(dict, MP_OBJ_NEW_QSTR(MP_QSTR_OFFSET_WIDTH)));

    mp_buffer_info_t widths_info, offsets_info, bitmaps_info;
    mp_get_buffer_raise(
        mp_obj_dict_get(dict, MP_OBJ_NEW_QSTR(MP_QSTR_WIDTHS)),
        &widths_info, MP_BUFFER_READ);
    mp_get_buffer_raise(
        mp_obj_dict_get(dict, MP_OBJ_NEW_QSTR(MP_QSTR_OFFSETS)),
        &offsets_info, MP_BUFFER_READ);
    mp_get_buffer_raise(
        mp_obj_dict_get(dict, MP_OBJ_NEW_QSTR(MP_QSTR_BITMAPS)),
        &bitmaps_info, MP_BUFFER_READ);

    const uint8_t *widths  = widths_info.buf;
    const uint8_t *offsets = offsets_info.buf;
    _bitmap_data            = bitmaps_info.buf;

    mp_obj_t map_obj = mp_obj_dict_get(dict, MP_OBJ_NEW_QSTR(MP_QSTR_MAP));
    GET_STR_DATA_LEN(map_obj, map_data, map_len);
    GET_STR_DATA_LEN(args[1], str_data, str_len);

    uint16_t fg16 = (uint16_t)fg;
    uint16_t bg16 = (uint16_t)(bg & 0xFFFF);
    bool transparent_bg = (bg == -1);

    const byte *s = str_data, *top = str_data + str_len;
    int cursor_x = x;

    while (s < top) {
        unichar ch = utf8_get_char(s);
        s = utf8_next_char(s);

        const byte *map_s = map_data, *map_top = map_data + map_len;
        uint16_t char_index = 0;

        while (map_s < map_top) {
            unichar map_ch = utf8_get_char(map_s);
            map_s = utf8_next_char(map_s);

            if (ch == map_ch) {
                uint8_t char_w = widths[char_index];

                // Decode offset
                _bs_bit = 0;
                switch (offset_width) {
                    case 1:
                        _bs_bit = offsets[char_index];
                        break;
                    case 2:
                        _bs_bit = (offsets[char_index * 2] << 8) |
                                   offsets[char_index * 2 + 1];
                        break;
                    case 3:
                        _bs_bit = (offsets[char_index * 3] << 16) |
                                  (offsets[char_index * 3 + 1] << 8) |
                                   offsets[char_index * 3 + 2];
                        break;
                }

                for (int row = 0; row < height; row++) {
                    int py = y + row;
                    if (py < 0 || py >= display_h) {
                        // Skip whole row but still consume bits
                        for (int col = 0; col < char_w; col++) _get_color(bpp);
                        continue;
                    }
                    for (int col = 0; col < char_w; col++) {
                        uint8_t pixel = _get_color(bpp);
                        int px = cursor_x + col;
                        if (px < 0 || px >= display_w) continue;
                        int idx = py * display_w + px;
                        if (pixel) {
                            dst[idx] = fg16;
                        } else if (!transparent_bg) {
                            dst[idx] = bg16;
                        }
                    }
                }
                cursor_x += char_w;
                break;
            }
            char_index++;
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_write_obj, 6, 7, animation_write);

// ─── text (fixed-width bitmap font) ──────────────────────────────────────────
// text(font, text, x, y, fg, display_buf {, bg=-1})

static mp_obj_t animation_text(size_t n_args, const mp_obj_t *args) {
    mp_obj_module_t *font = MP_OBJ_TO_PTR(args[0]);

    const uint8_t *source;
    size_t source_len;
    uint8_t single_char;

    if (mp_obj_is_int(args[1])) {
        single_char = (uint8_t)(mp_obj_get_int(args[1]) & 0xFF);
        source      = &single_char;
        source_len  = 1;
    } else {
        source     = (const uint8_t *)mp_obj_str_get_str(args[1]);
        source_len  = strlen((const char *)source);
    }

    int x  = mp_obj_get_int(args[2]);
    int y  = mp_obj_get_int(args[3]);
    int fg = mp_obj_get_int(args[4]);
    int bg = -1;
    if (n_args > 6) bg = mp_obj_get_int(args[6]);

    mp_buffer_info_t buf_info;
    mp_get_buffer_raise(args[5], &buf_info, MP_BUFFER_WRITE);
    uint16_t *dst = (uint16_t *)buf_info.buf;

    mp_obj_dict_t *dict = MP_OBJ_TO_PTR(font->globals);
    const uint8_t fw    = mp_obj_get_int(mp_obj_dict_get(dict, MP_OBJ_NEW_QSTR(MP_QSTR_WIDTH)));
    const uint8_t fh    = mp_obj_get_int(mp_obj_dict_get(dict, MP_OBJ_NEW_QSTR(MP_QSTR_HEIGHT)));
    const uint8_t first = mp_obj_get_int(mp_obj_dict_get(dict, MP_OBJ_NEW_QSTR(MP_QSTR_FIRST)));
    const uint8_t last  = mp_obj_get_int(mp_obj_dict_get(dict, MP_OBJ_NEW_QSTR(MP_QSTR_LAST)));

    mp_buffer_info_t font_info;
    mp_get_buffer_raise(
        mp_obj_dict_get(dict, MP_OBJ_NEW_QSTR(MP_QSTR_FONT)),
        &font_info, MP_BUFFER_READ);
    const uint8_t *font_data = font_info.buf;

    uint16_t fg16 = (uint16_t)fg;
    uint16_t bg16 = (uint16_t)(bg & 0xFFFF);
    bool transparent_bg = (bg == -1);
    uint8_t wide = fw / 8;
    int cursor_x = x;

    while (source_len--) {
        uint8_t ch = *source++;
        if (ch >= first && ch <= last) {
            uint16_t chr_idx = (ch - first) * (fh * wide);
            for (uint8_t row = 0; row < fh; row++) {
                int py = y + row;
                if (py < 0 || py >= display_h) continue;
                for (uint8_t byte_i = 0; byte_i < wide; byte_i++) {
                    uint8_t chr_byte = font_data[chr_idx + row * wide + byte_i];
                    for (int bit = 7; bit >= 0; bit--) {
                        int px = cursor_x + byte_i * 8 + (7 - bit);
                        if (px < 0 || px >= display_w) continue;
                        int idx = py * display_w + px;
                        if ((chr_byte >> bit) & 1) {
                            dst[idx] = fg16;
                        } else if (!transparent_bg) {
                            dst[idx] = bg16;
                        }
                    }
                }
            }
            cursor_x += fw;
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_text_obj, 6, 7, animation_text);

// ═══════════════════════════════════════════════════════════════════════════════
// ─── Module table ─────────────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

static const mp_rom_map_elem_t animation_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),            MP_ROM_QSTR(MP_QSTR_animation)                },
    // Display type
    { MP_ROM_QSTR(MP_QSTR_ESPLCD),              MP_ROM_PTR(&anim_display_type)                },
    // Slot system
    { MP_ROM_QSTR(MP_QSTR_set_display_size),    MP_ROM_PTR(&animation_set_display_size_obj)   },
    { MP_ROM_QSTR(MP_QSTR_clear_slots),         MP_ROM_PTR(&animation_clear_slots_obj)        },
    { MP_ROM_QSTR(MP_QSTR_set_slot),            MP_ROM_PTR(&animation_set_slot_obj)           },
    { MP_ROM_QSTR(MP_QSTR_update_slot),         MP_ROM_PTR(&animation_update_slot_obj)        },
    { MP_ROM_QSTR(MP_QSTR_update_slot_pos),     MP_ROM_PTR(&animation_update_slot_pos_obj)    },
    { MP_ROM_QSTR(MP_QSTR_update_slot_buf),     MP_ROM_PTR(&animation_update_slot_buf_obj)    },
    { MP_ROM_QSTR(MP_QSTR_enable_slot),         MP_ROM_PTR(&animation_enable_slot_obj)        },
    { MP_ROM_QSTR(MP_QSTR_set_slot_clip),       MP_ROM_PTR(&animation_set_slot_clip_obj)      },
    { MP_ROM_QSTR(MP_QSTR_draw_all),            MP_ROM_PTR(&animation_draw_all_obj)           },
    { MP_ROM_QSTR(MP_QSTR_fill_background),     MP_ROM_PTR(&animation_fill_background_obj)    },
    { MP_ROM_QSTR(MP_QSTR_flip_buf_horizontal), MP_ROM_PTR(&animation_flip_buf_horizontal_obj)},
    { MP_ROM_QSTR(MP_QSTR_flip_buf_vertical),   MP_ROM_PTR(&animation_flip_buf_vertical_obj)  },
    // Drawing
    { MP_ROM_QSTR(MP_QSTR_fill_rect),           MP_ROM_PTR(&animation_fill_rect_obj)          },
    { MP_ROM_QSTR(MP_QSTR_scroll),              MP_ROM_PTR(&animation_scroll_obj)             },
    { MP_ROM_QSTR(MP_QSTR_write),               MP_ROM_PTR(&animation_write_obj)              },
    { MP_ROM_QSTR(MP_QSTR_text),                MP_ROM_PTR(&animation_text_obj)               },
};
static MP_DEFINE_CONST_DICT(animation_module_globals, animation_module_globals_table);

const mp_obj_module_t mp_module_animation = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&animation_module_globals,
};

#if MICROPY_VERSION >= 0x011300
MP_REGISTER_MODULE(MP_QSTR_animation, mp_module_animation);
#else
MP_REGISTER_MODULE(MP_QSTR_animation, mp_module_animation, MODULE_ANIMATION_ENABLE);
#endif

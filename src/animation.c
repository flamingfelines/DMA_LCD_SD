/* Based off of Russ Hughes s3lcd software
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
/*
 * animation.c — Sprite compositing and drawing for MicroPython on ESP32-S3.
 *
 * Pipeline: fill_background → draw_all → tft.blit_buffer
 *
 * All drawing targets a Python bytearray (display_buf) that you manage.
 * Hardware init/blit lives in esp_lcd.c (ESPLCD).
 */

#include <stdlib.h>
#include <string.h>
#include "py/obj.h"
#include "py/runtime.h"
#include "py/mpconfig.h"
#include "py/unichar.h"
#include "esp_lcd.h"

// ─── Constants ────────────────────────────────────────────────────────────────

#define MAX_SLOTS    16
#define MAGIC_COLOR  58572   // RGB565 transparency key: RGB(231,154,99)

// ═══════════════════════════════════════════════════════════════════════════════
// ─── Slot system ──────────────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

typedef struct {
    uint8_t  *buf;
    int16_t   x, y, w, h;
    bool      enabled;
    uint8_t   opacity;         // 0 = invisible, 255 = fully opaque (default)
    // Vertical clip
    int16_t   clip_y;
    bool      clip_y_enabled;
    bool      clip_y_after;   // true = hide y >= clip_y  ("after")
                               // false = hide y <  clip_y  ("before")
    // Horizontal clip
    int16_t   clip_x;
    bool      clip_x_enabled;
    bool      clip_x_after;   // true = hide x >= clip_x  ("after")
                               // false = hide x <  clip_x  ("before")
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
        slots[i].opacity        = 255;
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
    slots[idx].opacity        = 255;
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

// ─── set_slot_opacity ────────────────────────────────────────────────────────
// set_slot_opacity(index, opacity)  opacity: 0 = invisible, 255 = fully opaque

static mp_obj_t animation_set_slot_opacity(mp_obj_t idx_in, mp_obj_t opacity_in) {
    int idx = mp_obj_get_int(idx_in);
    if (idx < 0 || idx >= MAX_SLOTS)
        mp_raise_ValueError(MP_ERROR_TEXT("slot index out of range"));
    int op = mp_obj_get_int(opacity_in);
    if (op < 0)   op = 0;
    if (op > 255) op = 255;
    slots[idx].opacity = (uint8_t)op;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(animation_set_slot_opacity_obj, animation_set_slot_opacity);

// ─── set_slot_clip ───────────────────────────────────────────────────────────
// set_slot_clip(index, clip_x, clip_x_dir, clip_y, clip_y_dir)
// clip_x / clip_y: pixel coordinate cutoff; 0 = disabled
// dir: "after"  → hide pixels AT or past the cutoff (>= cutoff)
//      "before" → hide pixels before the cutoff    (<  cutoff)

static mp_obj_t animation_set_slot_clip(size_t n_args, const mp_obj_t *args) {
    int idx = mp_obj_get_int(args[0]);
    if (idx < 0 || idx >= MAX_SLOTS)
        mp_raise_ValueError(MP_ERROR_TEXT("slot index out of range"));

    int16_t     clip_x = (int16_t)mp_obj_get_int(args[1]);
    const char *cx_dir = mp_obj_str_get_str(args[2]);
    int16_t     clip_y = (int16_t)mp_obj_get_int(args[3]);
    const char *cy_dir = mp_obj_str_get_str(args[4]);

    slots[idx].clip_x         = clip_x;
    slots[idx].clip_x_enabled = (clip_x != 0);
    slots[idx].clip_x_after   = (cx_dir[0] == 'a');  // 'a'fter vs 'b'efore

    slots[idx].clip_y         = clip_y;
    slots[idx].clip_y_enabled = (clip_y != 0);
    slots[idx].clip_y_after   = (cy_dir[0] == 'a');

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_set_slot_clip_obj, 5, 5, animation_set_slot_clip);

// ─── Internal blit ───────────────────────────────────────────────────────────

static void blit_slot(sprite_slot_t *slot, uint8_t *dst) {
    uint8_t *src     = slot->buf;
    int16_t  sw      = slot->w;
    int16_t  sh      = slot->h;
    int16_t  ox      = slot->x;
    int16_t  oy      = slot->y;
    uint8_t  opacity = slot->opacity;

    for (int row = 0; row < sh; row++) {
        int target_row = oy + row;
        if (target_row < 0 || target_row >= display_h) continue;

        if (slot->clip_y_enabled) {
            if ( slot->clip_y_after && target_row >= slot->clip_y) continue;
            if (!slot->clip_y_after && target_row <  slot->clip_y) continue;
        }

        int src_row_base = row * sw * 2;
        int dst_row_base = target_row * display_w * 2;

        for (int col = 0; col < sw; col++) {
            int target_col = ox + col;
            if (target_col < 0 || target_col >= display_w) continue;

            if (slot->clip_x_enabled) {
                if ( slot->clip_x_after && target_col >= slot->clip_x) continue;
                if (!slot->clip_x_after && target_col <  slot->clip_x) continue;
            }

            int si    = src_row_base + col * 2;
            int color = (src[si] << 8) | src[si + 1];
            if (color == MAGIC_COLOR) continue;

            int di = dst_row_base + target_col * 2;

            if (opacity == 255) {
                // Fast path — fully opaque, direct copy
                dst[di]     = src[si];
                dst[di + 1] = src[si + 1];
            } else if (opacity > 0) {
                // Blend path — unpack RGB565, lerp, repack
                // Note: lsb_first display — bytes are stored swapped
                uint16_t s16 = (src[si] << 8) | src[si + 1];
                uint16_t d16 = (dst[di] << 8) | dst[di + 1];

                uint32_t sr = (s16 >> 11) & 0x1F;
                uint32_t sg = (s16 >>  5) & 0x3F;
                uint32_t sb =  s16        & 0x1F;

                uint32_t dr = (d16 >> 11) & 0x1F;
                uint32_t dg = (d16 >>  5) & 0x3F;
                uint32_t db =  d16        & 0x1F;

                uint32_t a  = opacity;
                uint32_t ia = 255 - a;

                uint32_t or_ = (sr * a + dr * ia) >> 8;
                uint32_t og  = (sg * a + dg * ia) >> 8;
                uint32_t ob  = (sb * a + db * ia) >> 8;

                uint16_t out = (uint16_t)((or_ << 11) | (og << 5) | ob);
                dst[di]     = (uint8_t)(out >> 8);
                dst[di + 1] = (uint8_t)(out & 0xFF);
            }
            // opacity == 0: skip pixel entirely
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
    int      w      = mp_obj_get_int(args[2]);
    int      h      = mp_obj_get_int(args[3]);
    uint8_t *src    = (uint8_t *)src_info.buf;
    uint8_t *dst    = (uint8_t *)dst_info.buf;
    int      stride = w * 2;
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
    int      w      = mp_obj_get_int(args[2]);
    int      h      = mp_obj_get_int(args[3]);
    uint8_t *src    = (uint8_t *)src_info.buf;
    uint8_t *dst    = (uint8_t *)dst_info.buf;
    int      stride = w * 2;
    for (int row = 0; row < h; row++) {
        memcpy(dst + row * stride, src + (h - 1 - row) * stride, stride);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_flip_buf_vertical_obj, 4, 4, animation_flip_buf_vertical);

// ═══════════════════════════════════════════════════════════════════════════════
// ─── Drawing functions ────────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

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
// scroll(display_buf, dx, dy {, fill_color=0})
// dx > 0 = right,  dx < 0 = left
// dy > 0 = down,   dy < 0 = up

static mp_obj_t animation_scroll(size_t n_args, const mp_obj_t *args) {
    mp_buffer_info_t info;
    mp_get_buffer_raise(args[0], &info, MP_BUFFER_WRITE);
    uint16_t *buf = (uint16_t *)info.buf;

    int dx   = mp_obj_get_int(args[1]);
    int dy   = mp_obj_get_int(args[2]);
    int fill = (n_args > 3) ? mp_obj_get_int(args[3]) : 0;

    int W = display_w;
    int H = display_h;

    // Iterate in the direction of the scroll to avoid src/dst overlap
    int y_start, y_end, y_step;
    int x_start, x_end, x_step;

    if (dy >= 0) { y_start = H - 1; y_end = -1; y_step = -1; }
    else         { y_start = 0;     y_end =  H; y_step =  1; }
    if (dx >= 0) { x_start = W - 1; x_end = -1; x_step = -1; }
    else         { x_start = 0;     x_end =  W; x_step =  1; }

    for (int y = y_start; y != y_end; y += y_step) {
        for (int x = x_start; x != x_end; x += x_step) {
            int src_x = x - dx;
            int src_y = y - dy;
            if (src_x >= 0 && src_x < W && src_y >= 0 && src_y < H)
                buf[y * W + x] = buf[src_y * W + src_x];
            else
                buf[y * W + x] = (uint16_t)fill;
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_scroll_obj, 3, 4, animation_scroll);

// ─── write (proportional bitmap font) ────────────────────────────────────────
// write(font, text, x, y, fg, display_buf {, bg=-1})
// bg = -1 means transparent background (default)

static uint32_t _bs_bit    = 0;
static uint8_t *_bmap_data = NULL;

static uint8_t _get_color(uint8_t bpp) {
    uint8_t color = 0;
    for (int i = 0; i < bpp; i++) {
        color <<= 1;
        color |= (_bmap_data[_bs_bit / 8] & (1 << (7 - (_bs_bit % 8)))) > 0;
        _bs_bit++;
    }
    return color;
}

static mp_obj_t animation_write(size_t n_args, const mp_obj_t *args) {
    // write(font, text, x, y, fg, display_buf {, bg=-1})
    mp_obj_module_t *font = MP_OBJ_TO_PTR(args[0]);

    const char *text;
    static char single[2] = {0, 0};
    if (mp_obj_is_int(args[1])) {
        single[0] = (char)(mp_obj_get_int(args[1]) & 0xFF);
        text = single;
    } else {
        text = mp_obj_str_get_str(args[1]);
    }

    int  x  = mp_obj_get_int(args[2]);
    int  y  = mp_obj_get_int(args[3]);
    int  fg = mp_obj_get_int(args[4]);
    int  bg = (n_args > 6) ? mp_obj_get_int(args[6]) : -1;
    bool transparent_bg = (bg == -1);

    mp_buffer_info_t buf_info;
    mp_get_buffer_raise(args[5], &buf_info, MP_BUFFER_WRITE);
    uint16_t *dst = (uint16_t *)buf_info.buf;

    mp_obj_dict_t *dict = MP_OBJ_TO_PTR(font->globals);

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
    _bmap_data              = bitmaps_info.buf;

    mp_obj_t map_obj = mp_obj_dict_get(dict, MP_OBJ_NEW_QSTR(MP_QSTR_MAP));
    GET_STR_DATA_LEN(map_obj,  map_data, map_len);
    GET_STR_DATA_LEN(args[1],  str_data, str_len);

    uint16_t fg16 = (uint16_t)fg;
    uint16_t bg16 = (uint16_t)(bg & 0xFFFF);
    int cursor_x  = x;

    const byte *s = str_data, *top = str_data + str_len;
    while (s < top) {
        unichar ch = utf8_get_char(s);
        s = utf8_next_char(s);

        const byte *map_s   = map_data;
        const byte *map_top = map_data + map_len;
        uint16_t    char_index = 0;

        while (map_s < map_top) {
            unichar map_ch = utf8_get_char(map_s);
            map_s = utf8_next_char(map_s);

            if (ch == map_ch) {
                uint8_t char_w = widths[char_index];

                // Decode bit offset into bitmap data
                _bs_bit = 0;
                switch (offset_width) {
                    case 1:
                        _bs_bit = offsets[char_index];
                        break;
                    case 2:
                        _bs_bit = ((uint32_t)offsets[char_index * 2]     << 8) |
                                              offsets[char_index * 2 + 1];
                        break;
                    case 3:
                        _bs_bit = ((uint32_t)offsets[char_index * 3]     << 16) |
                                  ((uint32_t)offsets[char_index * 3 + 1] <<  8) |
                                             offsets[char_index * 3 + 2];
                        break;
                }

                for (int row = 0; row < height; row++) {
                    int py = y + row;
                    if (py < 0 || py >= display_h) {
                        // consume bits for this row even if off-screen
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
// Font format: WIDTH, HEIGHT, FIRST, LAST, FONT keys (fixed-width bitmaps)

static mp_obj_t animation_text(size_t n_args, const mp_obj_t *args) {
    mp_obj_module_t *font = MP_OBJ_TO_PTR(args[0]);

    const uint8_t *source;
    size_t         source_len;
    uint8_t        single_char;

    if (mp_obj_is_int(args[1])) {
        single_char = (uint8_t)(mp_obj_get_int(args[1]) & 0xFF);
        source      = &single_char;
        source_len  = 1;
    } else {
        source     = (const uint8_t *)mp_obj_str_get_str(args[1]);
        source_len  = strlen((const char *)source);
    }

    int  x  = mp_obj_get_int(args[2]);
    int  y  = mp_obj_get_int(args[3]);
    int  fg = mp_obj_get_int(args[4]);
    int  bg = (n_args > 6) ? mp_obj_get_int(args[6]) : -1;
    bool transparent_bg = (bg == -1);

    mp_buffer_info_t buf_info;
    mp_get_buffer_raise(args[5], &buf_info, MP_BUFFER_WRITE);
    uint16_t *dst = (uint16_t *)buf_info.buf;

    mp_obj_dict_t *dict  = MP_OBJ_TO_PTR(font->globals);
    const uint8_t  fw    = mp_obj_get_int(mp_obj_dict_get(dict, MP_OBJ_NEW_QSTR(MP_QSTR_WIDTH)));
    const uint8_t  fh    = mp_obj_get_int(mp_obj_dict_get(dict, MP_OBJ_NEW_QSTR(MP_QSTR_HEIGHT)));
    const uint8_t  first = mp_obj_get_int(mp_obj_dict_get(dict, MP_OBJ_NEW_QSTR(MP_QSTR_FIRST)));
    const uint8_t  last  = mp_obj_get_int(mp_obj_dict_get(dict, MP_OBJ_NEW_QSTR(MP_QSTR_LAST)));

    mp_buffer_info_t font_info;
    mp_get_buffer_raise(
        mp_obj_dict_get(dict, MP_OBJ_NEW_QSTR(MP_QSTR_FONT)),
        &font_info, MP_BUFFER_READ);
    const uint8_t *font_data = font_info.buf;

    uint16_t fg16  = (uint16_t)fg;
    uint16_t bg16  = (uint16_t)(bg & 0xFFFF);
    uint8_t  wide  = fw / 8;
    int      cursor_x = x;

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
    { MP_ROM_QSTR(MP_QSTR___name__),            MP_ROM_QSTR(MP_QSTR_animation)                 },
    // Slot system
    { MP_ROM_QSTR(MP_QSTR_set_display_size),    MP_ROM_PTR(&animation_set_display_size_obj)    },
    { MP_ROM_QSTR(MP_QSTR_clear_slots),         MP_ROM_PTR(&animation_clear_slots_obj)         },
    { MP_ROM_QSTR(MP_QSTR_set_slot),            MP_ROM_PTR(&animation_set_slot_obj)            },
    { MP_ROM_QSTR(MP_QSTR_update_slot),         MP_ROM_PTR(&animation_update_slot_obj)         },
    { MP_ROM_QSTR(MP_QSTR_update_slot_pos),     MP_ROM_PTR(&animation_update_slot_pos_obj)     },
    { MP_ROM_QSTR(MP_QSTR_update_slot_buf),     MP_ROM_PTR(&animation_update_slot_buf_obj)     },
    { MP_ROM_QSTR(MP_QSTR_enable_slot),         MP_ROM_PTR(&animation_enable_slot_obj)         },
    { MP_ROM_QSTR(MP_QSTR_set_slot_opacity),    MP_ROM_PTR(&animation_set_slot_opacity_obj)    },
    { MP_ROM_QSTR(MP_QSTR_set_slot_clip),       MP_ROM_PTR(&animation_set_slot_clip_obj)       },
    { MP_ROM_QSTR(MP_QSTR_draw_all),            MP_ROM_PTR(&animation_draw_all_obj)            },
    { MP_ROM_QSTR(MP_QSTR_fill_background),     MP_ROM_PTR(&animation_fill_background_obj)     },
    { MP_ROM_QSTR(MP_QSTR_flip_buf_horizontal), MP_ROM_PTR(&animation_flip_buf_horizontal_obj) },
    { MP_ROM_QSTR(MP_QSTR_flip_buf_vertical),   MP_ROM_PTR(&animation_flip_buf_vertical_obj)   },
    // Drawing
    { MP_ROM_QSTR(MP_QSTR_fill_rect),           MP_ROM_PTR(&animation_fill_rect_obj)           },
    { MP_ROM_QSTR(MP_QSTR_scroll),              MP_ROM_PTR(&animation_scroll_obj)              },
    { MP_ROM_QSTR(MP_QSTR_write),               MP_ROM_PTR(&animation_write_obj)               },
    { MP_ROM_QSTR(MP_QSTR_text),                MP_ROM_PTR(&animation_text_obj)                },
};
static MP_DEFINE_CONST_DICT(animation_module_globals, animation_module_globals_table);

const mp_obj_module_t mp_module_animation = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&animation_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_animation, mp_module_animation);

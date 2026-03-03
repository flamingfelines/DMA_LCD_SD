/*
 * animation.c - Sprite compositing module for MicroPython on ESP32-S3
 * 
 * Provides a slot-based sprite drawing system for efficient frame compositing.
 * Slots are drawn in order (0 = bottom, N = top).
 */

#include <stdlib.h>
#include <string.h>
#include "py/obj.h"
#include "py/runtime.h"
#include "py/mpconfig.h"

#define MAX_SLOTS 16
#define MAGIC_COLOR 58572  // RGB(231, 154, 99) - transparency key color

// ─── Slot Storage ────────────────────────────────────────────────────────────

typedef struct {
    uint8_t  *buf;       // pointer into the Python bytearray
    int16_t   x;
    int16_t   y;
    int16_t   w;
    int16_t   h;
    bool      enabled;
    bool      clipped;
    int16_t   clip_bottom;
} sprite_slot_t;

static sprite_slot_t slots[MAX_SLOTS];
static int16_t display_w = 240;
static int16_t display_h = 240;

// ─── set_display_size(w, h) ───────────────────────────────────────────────────
// Call once at init so slots don't need bg_w/bg_h passed every draw call.

static mp_obj_t animation_set_display_size(mp_obj_t w_in, mp_obj_t h_in) {
    display_w = (int16_t)mp_obj_get_int(w_in);
    display_h = (int16_t)mp_obj_get_int(h_in);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(animation_set_display_size_obj, animation_set_display_size);

// ─── clear_slots() ───────────────────────────────────────────────────────────
// Disables all slots. Call on scene transitions.

static mp_obj_t animation_clear_slots(void) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        slots[i].enabled    = false;
        slots[i].buf        = NULL;
        slots[i].clipped    = false;
        slots[i].clip_bottom = 0;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(animation_clear_slots_obj, animation_clear_slots);

// ─── set_slot(index, buf, x, y, w, h) ────────────────────────────────────────
// Register a sprite in a slot. Enables it automatically.

static mp_obj_t animation_set_slot(size_t n_args, const mp_obj_t *args) {
    int idx = mp_obj_get_int(args[0]);
    if (idx < 0 || idx >= MAX_SLOTS) {
        mp_raise_ValueError(MP_ERROR_TEXT("slot index out of range"));
    }

    mp_buffer_info_t buf_info;
    mp_get_buffer_raise(args[1], &buf_info, MP_BUFFER_READ);

    slots[idx].buf        = (uint8_t *)buf_info.buf;
    slots[idx].x          = (int16_t)mp_obj_get_int(args[2]);
    slots[idx].y          = (int16_t)mp_obj_get_int(args[3]);
    slots[idx].w          = (int16_t)mp_obj_get_int(args[4]);
    slots[idx].h          = (int16_t)mp_obj_get_int(args[5]);
    slots[idx].enabled    = true;
    slots[idx].clipped    = false;
    slots[idx].clip_bottom = 0;

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_set_slot_obj, 6, 6, animation_set_slot);

// ─── update_slot(index, buf, x, y) ───────────────────────────────────────────
// Update the frame buffer and position of an existing slot.
// w/h stay the same — use set_slot if you need to change those.

static mp_obj_t animation_update_slot(size_t n_args, const mp_obj_t *args) {
    int idx = mp_obj_get_int(args[0]);
    if (idx < 0 || idx >= MAX_SLOTS) {
        mp_raise_ValueError(MP_ERROR_TEXT("slot index out of range"));
    }

    mp_buffer_info_t buf_info;
    mp_get_buffer_raise(args[1], &buf_info, MP_BUFFER_READ);

    slots[idx].buf = (uint8_t *)buf_info.buf;
    slots[idx].x   = (int16_t)mp_obj_get_int(args[2]);
    slots[idx].y   = (int16_t)mp_obj_get_int(args[3]);

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_update_slot_obj, 4, 4, animation_update_slot);

// ─── update_slot_pos(index, x, y) ────────────────────────────────────────────
// Update only the position of a slot. Useful for stationary sprites that
// swap frames without moving (update_slot) vs sprites that only move.

static mp_obj_t animation_update_slot_pos(size_t n_args, const mp_obj_t *args) {
    int idx = mp_obj_get_int(args[0]);
    if (idx < 0 || idx >= MAX_SLOTS) {
        mp_raise_ValueError(MP_ERROR_TEXT("slot index out of range"));
    }
    slots[idx].x = (int16_t)mp_obj_get_int(args[1]);
    slots[idx].y = (int16_t)mp_obj_get_int(args[2]);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_update_slot_pos_obj, 3, 3, animation_update_slot_pos);

// ─── update_slot_buf(index, buf) ─────────────────────────────────────────────
// Update only the frame buffer of a slot. Useful when position is fixed
// but the frame changes (e.g. animation frames on a stationary NPC).

static mp_obj_t animation_update_slot_buf(size_t n_args, const mp_obj_t *args) {
    int idx = mp_obj_get_int(args[0]);
    if (idx < 0 || idx >= MAX_SLOTS) {
        mp_raise_ValueError(MP_ERROR_TEXT("slot index out of range"));
    }
    mp_buffer_info_t buf_info;
    mp_get_buffer_raise(args[1], &buf_info, MP_BUFFER_READ);
    slots[idx].buf = (uint8_t *)buf_info.buf;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_update_slot_buf_obj, 2, 2, animation_update_slot_buf);

// ─── set_slot_clip(index, clip_bottom) ───────────────────────────────────────
// Enable vertical clipping on a slot. Pixels at or below clip_bottom are skipped.
// Pass clip_bottom=0 to disable clipping.

static mp_obj_t animation_set_slot_clip(size_t n_args, const mp_obj_t *args) {
    int idx = mp_obj_get_int(args[0]);
    if (idx < 0 || idx >= MAX_SLOTS) {
        mp_raise_ValueError(MP_ERROR_TEXT("slot index out of range"));
    }
    int16_t clip = (int16_t)mp_obj_get_int(args[1]);
    slots[idx].clip_bottom = clip;
    slots[idx].clipped     = (clip > 0);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_set_slot_clip_obj, 2, 2, animation_set_slot_clip);

// ─── enable_slot(index, enabled) ─────────────────────────────────────────────
// Show or hide a slot without clearing its data.

static mp_obj_t animation_enable_slot(size_t n_args, const mp_obj_t *args) {
    int idx = mp_obj_get_int(args[0]);
    if (idx < 0 || idx >= MAX_SLOTS) {
        mp_raise_ValueError(MP_ERROR_TEXT("slot index out of range"));
    }
    slots[idx].enabled = mp_obj_is_true(args[1]);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_enable_slot_obj, 2, 2, animation_enable_slot);

// ─── Internal blit ───────────────────────────────────────────────────────────

static void blit_slot(sprite_slot_t *slot, uint8_t *dst) {
    uint8_t *src     = slot->buf;
    int16_t  sw      = slot->w;
    int16_t  sh      = slot->h;
    int16_t  ox      = slot->x;
    int16_t  oy      = slot->y;
    bool     clipped = slot->clipped;
    int16_t  clip_b  = slot->clip_bottom;

    for (int row = 0; row < sh; row++) {
        int target_row = oy + row;
        if (target_row < 0 || target_row >= display_h) continue;
        if (clipped && target_row >= clip_b)            continue;

        int src_row_base = row * sw * 2;
        int dst_row_base = target_row * display_w * 2;

        for (int col = 0; col < sw; col++) {
            int target_col = ox + col;
            if (target_col < 0 || target_col >= display_w) continue;

            int   si    = src_row_base + col * 2;
            int   color = (src[si] << 8) | src[si + 1];

            if (color != MAGIC_COLOR) {
                int di      = dst_row_base + target_col * 2;
                dst[di]     = src[si];
                dst[di + 1] = src[si + 1];
            }
        }
    }
}

// ─── draw_all(display_buf) ────────────────────────────────────────────────────
// Composite all enabled slots in order onto display_buf.
// Slot 0 is drawn first (bottom), MAX_SLOTS-1 last (top).
// The background should be slot 0 with x=0, y=0, w=display_w, h=display_h
// and no transparency (or just use fill_background before calling draw_all
// and start sprites at slot 1).

static mp_obj_t animation_draw_all(mp_obj_t display_buf_in) {
    mp_buffer_info_t dst_info;
    mp_get_buffer_raise(display_buf_in, &dst_info, MP_BUFFER_WRITE);
    uint8_t *dst = (uint8_t *)dst_info.buf;

    for (int i = 0; i < MAX_SLOTS; i++) {
        if (!slots[i].enabled || slots[i].buf == NULL) continue;
        blit_slot(&slots[i], dst);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(animation_draw_all_obj, animation_draw_all);

// ─── fill_background(display_buf, bg_data) ───────────────────────────────────
// Fast memcpy of bg_data into display_buf.
// Call this before draw_all if your background is not in a slot,
// or if you want to reset the buffer each frame before compositing.

static mp_obj_t animation_fill_background(mp_obj_t display_buf_in, mp_obj_t bg_in) {
    mp_buffer_info_t dst_info, src_info;
    mp_get_buffer_raise(display_buf_in, &dst_info, MP_BUFFER_WRITE);
    mp_get_buffer_raise(bg_in,          &src_info, MP_BUFFER_READ);

    size_t copy_len = dst_info.len < src_info.len ? dst_info.len : src_info.len;
    memcpy(dst_info.buf, src_info.buf, copy_len);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(animation_fill_background_obj, animation_fill_background);

// ─── flip_buf_horizontal(src, dst, w, h) ─────────────────────────────────────

static mp_obj_t animation_flip_buf_horizontal(size_t n_args, const mp_obj_t *args) {
    mp_buffer_info_t src_info, dst_info;
    mp_get_buffer_raise(args[0], &src_info, MP_BUFFER_READ);
    mp_get_buffer_raise(args[1], &dst_info, MP_BUFFER_WRITE);

    int w = mp_obj_get_int(args[2]);
    int h = mp_obj_get_int(args[3]);

    uint8_t *src = (uint8_t *)src_info.buf;
    uint8_t *dst = (uint8_t *)dst_info.buf;
    int row_stride = w * 2;

    for (int row = 0; row < h; row++) {
        int row_base = row * row_stride;
        for (int col = 0; col < w; col++) {
            int si = row_base + col * 2;
            int di = row_base + (w - 1 - col) * 2;
            dst[di]     = src[si];
            dst[di + 1] = src[si + 1];
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_flip_buf_horizontal_obj, 4, 4, animation_flip_buf_horizontal);

// ─── flip_buf_vertical(src, dst, w, h) ───────────────────────────────────────

static mp_obj_t animation_flip_buf_vertical(size_t n_args, const mp_obj_t *args) {
    mp_buffer_info_t src_info, dst_info;
    mp_get_buffer_raise(args[0], &src_info, MP_BUFFER_READ);
    mp_get_buffer_raise(args[1], &dst_info, MP_BUFFER_WRITE);

    int w = mp_obj_get_int(args[2]);
    int h = mp_obj_get_int(args[3]);

    uint8_t *src = (uint8_t *)src_info.buf;
    uint8_t *dst = (uint8_t *)dst_info.buf;
    int row_stride = w * 2;

    for (int row = 0; row < h; row++) {
        memcpy(
            dst + row * row_stride,
            src + (h - 1 - row) * row_stride,
            row_stride
        );
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(animation_flip_buf_vertical_obj, 4, 4, animation_flip_buf_vertical);

// ─── Module table ─────────────────────────────────────────────────────────────

static const mp_rom_map_elem_t animation_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),           MP_ROM_QSTR(MP_QSTR_animation)             },
    { MP_ROM_QSTR(MP_QSTR_set_display_size),   MP_ROM_PTR(&animation_set_display_size_obj)},
    { MP_ROM_QSTR(MP_QSTR_clear_slots),        MP_ROM_PTR(&animation_clear_slots_obj)     },
    { MP_ROM_QSTR(MP_QSTR_set_slot),           MP_ROM_PTR(&animation_set_slot_obj)        },
    { MP_ROM_QSTR(MP_QSTR_update_slot),        MP_ROM_PTR(&animation_update_slot_obj)     },
    { MP_ROM_QSTR(MP_QSTR_update_slot_pos),    MP_ROM_PTR(&animation_update_slot_pos_obj) },
    { MP_ROM_QSTR(MP_QSTR_update_slot_buf),    MP_ROM_PTR(&animation_update_slot_buf_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_slot_clip),      MP_ROM_PTR(&animation_set_slot_clip_obj)   },
    { MP_ROM_QSTR(MP_QSTR_enable_slot),        MP_ROM_PTR(&animation_enable_slot_obj)     },
    { MP_ROM_QSTR(MP_QSTR_draw_all),           MP_ROM_PTR(&animation_draw_all_obj)        },
    { MP_ROM_QSTR(MP_QSTR_fill_background),    MP_ROM_PTR(&animation_fill_background_obj) },
    { MP_ROM_QSTR(MP_QSTR_flip_buf_horizontal),MP_ROM_PTR(&animation_flip_buf_horizontal_obj)},
    { MP_ROM_QSTR(MP_QSTR_flip_buf_vertical),  MP_ROM_PTR(&animation_flip_buf_vertical_obj) },
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

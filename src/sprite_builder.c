/*
 * sprite_builder.c — Pose assembly for MicroPython on ESP32-S3.
 *
 * Composites pre-read part frame buffers into a single RGB565 destination.
 * File I/O is handled by Python (MicroPython VFS) before calling build().
 *
 * Interface:
 *   sprite_builder.build(dest_buf, layers, part_w, part_h)
 *
 *   dest_buf : writable bytearray, exactly part_w * part_h * 2 bytes
 *   layers   : tuple of (frame_buf, flip, dx, dy)
 *              frame_buf is a bytes/bytearray of exactly part_w * part_h * 2
 *   part_w   : width  of one part frame in pixels (typically 33)
 *   part_h   : height of one part frame in pixels (typically 33)
 */

#include <string.h>
#include "py/obj.h"
#include "py/runtime.h"

// ─── Constants ────────────────────────────────────────────────────────────────

#define MAX_LAYERS  13
#define MAX_PART_W  33
#define MAX_PART_H  33
#define MAGIC_COLOR 0xE49C      // RGB565 transparency key — must match animation.c

// ─── build ────────────────────────────────────────────────────────────────────

static mp_obj_t sprite_builder_build(size_t n_args, const mp_obj_t *args) {

    if (n_args != 4)
        mp_raise_TypeError(MP_ERROR_TEXT("build requires 4 args: dest_buf, layers, part_w, part_h"));

    // ── dest_buf ──────────────────────────────────────────────────────────────
    mp_buffer_info_t dest_info;
    mp_get_buffer_raise(args[0], &dest_info, MP_BUFFER_WRITE);
    uint8_t *dest = (uint8_t *)dest_info.buf;

    // ── layer list ────────────────────────────────────────────────────────────
    size_t    n_layers = 0;
    mp_obj_t *layer_items_raw;
    mp_obj_get_array(args[1], &n_layers, &layer_items_raw);
    if (n_layers > MAX_LAYERS)
        mp_raise_ValueError(MP_ERROR_TEXT("too many layers"));

    // ── dimensions ───────────────────────────────────────────────────────────
    int part_w = mp_obj_get_int(args[2]);
    int part_h = mp_obj_get_int(args[3]);
    if (part_w <= 0 || part_w > MAX_PART_W || part_h <= 0 || part_h > MAX_PART_H)
        mp_raise_ValueError(MP_ERROR_TEXT("part dimensions out of range (max 33x33)"));

    int frame_bytes = part_w * part_h * 2;

    if ((int)dest_info.len < frame_bytes)
        mp_raise_ValueError(MP_ERROR_TEXT("dest_buf too small"));

    // ── Stack flip buffer — no heap allocation ────────────────────────────────
    uint8_t flip_buf[MAX_PART_W * MAX_PART_H * 2];

    // ── Pre-fill dest with MAGIC_COLOR ────────────────────────────────────────
    uint8_t magic_hi = (MAGIC_COLOR >> 8) & 0xFF;
    uint8_t magic_lo =  MAGIC_COLOR       & 0xFF;
    for (int i = 0; i < frame_bytes; i += 2) {
        dest[i]     = magic_hi;
        dest[i + 1] = magic_lo;
    }

    // ── Composite layers bottom to top ────────────────────────────────────────
    for (size_t li = 0; li < n_layers; li++) {

        // ── Unpack layer tuple: (frame_buf, flip, dx, dy) ─────────────────────
        size_t    layer_len;
        mp_obj_t *layer_fields;
        mp_obj_get_array(layer_items_raw[li], &layer_len, &layer_fields);
        if (layer_len != 4)
            mp_raise_ValueError(
                MP_ERROR_TEXT("each layer must be (frame_buf, flip, dx, dy)"));

        mp_buffer_info_t frame_info;
        mp_get_buffer_raise(layer_fields[0], &frame_info, MP_BUFFER_READ);

        if ((int)frame_info.len < frame_bytes)
            mp_raise_ValueError(MP_ERROR_TEXT("layer frame_buf too small"));

        const uint8_t *frame_src = (const uint8_t *)frame_info.buf;
        bool           flip      = mp_obj_is_true(layer_fields[1]);
        int            dx        = mp_obj_get_int(layer_fields[2]);
        int            dy        = mp_obj_get_int(layer_fields[3]);

        // ── Horizontal flip into flip_buf if requested ────────────────────────
        const uint8_t *src;
        if (flip) {
            for (int row = 0; row < part_h; row++) {
                for (int col = 0; col < part_w; col++) {
                    int si = (row * part_w + col)                * 2;
                    int di = (row * part_w + (part_w - 1 - col)) * 2;
                    flip_buf[di]     = frame_src[si];
                    flip_buf[di + 1] = frame_src[si + 1];
                }
            }
            src = flip_buf;
        } else {
            src = frame_src;
        }

        // ── Composite src onto dest — MAGIC_COLOR transparent, dx/dy offset ───
        for (int row = 0; row < part_h; row++) {
            int dest_row = row + dy;
            if (dest_row < 0 || dest_row >= part_h) continue;

            int src_base  = row      * part_w * 2;
            int dest_base = dest_row * part_w * 2;

            for (int col = 0; col < part_w; col++) {
                int dest_col = col + dx;
                if (dest_col < 0 || dest_col >= part_w) continue;

                int si    = src_base  + col      * 2;
                int di    = dest_base + dest_col * 2;

                uint16_t pixel = ((uint16_t)src[si] << 8) | src[si + 1];
                if (pixel == MAGIC_COLOR) continue;

                dest[di]     = src[si];
                dest[di + 1] = src[si + 1];
            }
        }
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(
    sprite_builder_build_obj, 4, 4, sprite_builder_build);

// ─── Module table ─────────────────────────────────────────────────────────────

static const mp_rom_map_elem_t sprite_builder_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_sprite_builder) },
    { MP_ROM_QSTR(MP_QSTR_build),    MP_ROM_PTR(&sprite_builder_build_obj) },
};
static MP_DEFINE_CONST_DICT(
    sprite_builder_module_globals,
    sprite_builder_module_globals_table);

const mp_obj_module_t mp_module_sprite_builder = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&sprite_builder_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_sprite_builder, mp_module_sprite_builder);

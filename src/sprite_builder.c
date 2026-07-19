/*
 * sprite_builder.c — Pose assembly for MicroPython on ESP32-S3.
 *
 * Reads part frames directly from SD card files and composites them
 * into a single RGB565 destination buffer in one call. No intermediate
 * Python sheet buffers needed.
 *
 * Interface:
 *   sprite_builder.build(dest_buf, layers, part_w, part_h)
 *
 *   dest_buf : writable bytearray, exactly part_w * part_h * 2 bytes
 *   layers   : tuple of (path_str, part_index, flip, dx, dy)
 *   part_w   : width  of one part frame in pixels (typically 32)
 *   part_h   : height of one part frame in pixels (typically 32)
 *
 * Part sheet format:
 *   Raw RGB565, no padding. Frame N starts at byte offset N * W * H * 2.
 *
 * Transparency:
 *   MAGIC_COLOR (0xE493) pixels are never written to dest_buf.
 *
 * Destination is pre-filled with MAGIC_COLOR so the result can itself
 * be used as a transparent sprite in the animation slot system.
 *
 * File I/O:
 *   Opens each part file via newlib fopen (works against MicroPython VFS
 *   mount). Seeks directly to the required frame offset — never loads a
 *   full sheet into memory. Stack-allocates one frame buffer and one flip
 *   buffer; no heap allocation.
 */

#include <stdio.h>
#include <string.h>
#include "py/obj.h"
#include "py/runtime.h"

// ─── Constants ────────────────────────────────────────────────────────────────

#define MAX_LAYERS  13          // head + torso + 2 arms + 2 feet + wings +
                                // tail + 2 eyes + mouth + accessory + 1 spare
#define MAX_PART_W  33
#define MAX_PART_H  33
#define MAGIC_COLOR 0xE493      // RGB565 transparency key — must match animation.c

// ─── build ────────────────────────────────────────────────────────────────────

static mp_obj_t sprite_builder_build(size_t n_args, const mp_obj_t *args) {

    if (n_args != 4)
        mp_raise_TypeError(MP_ERROR_TEXT("build requires 4 args: dest_buf, layers, part_w, part_h"));

    // ── dest_buf ──────────────────────────────────────────────────────────────
    mp_buffer_info_t dest_info;
    mp_get_buffer_raise(args[0], &dest_info, MP_BUFFER_WRITE);
    uint8_t *dest = (uint8_t *)dest_info.buf;

    // ── layer list ────────────────────────────────────────────────────────────
    size_t    n_layers  = 0;
    mp_obj_t *layer_items_raw;
    mp_obj_get_array(args[1], &n_layers, &layer_items_raw);
    if (n_layers > MAX_LAYERS)
        mp_raise_ValueError(MP_ERROR_TEXT("too many layers"));

    // ── dimensions ───────────────────────────────────────────────────────────
    int part_w = mp_obj_get_int(args[2]);
    int part_h = mp_obj_get_int(args[3]);
    if (part_w <= 0 || part_w > MAX_PART_W || part_h <= 0 || part_h > MAX_PART_H)
        mp_raise_ValueError(MP_ERROR_TEXT("part dimensions out of range (max 32x32)"));

    int frame_bytes = part_w * part_h * 2;

    if ((int)dest_info.len < frame_bytes)
        mp_raise_ValueError(MP_ERROR_TEXT("dest_buf too small for part_w * part_h * 2"));

    // ── Stack buffers — no heap allocation ───────────────────────────────────
    uint8_t frame_buf[MAX_PART_W * MAX_PART_H * 2];   // raw frame from file
    uint8_t flip_buf [MAX_PART_W * MAX_PART_H * 2];   // mirrored copy if needed

    // ── Pre-fill dest with MAGIC_COLOR ────────────────────────────────────────
    // Unused pixels stay transparent when this buffer is later used as a sprite.
    uint8_t magic_hi = (MAGIC_COLOR >> 8) & 0xFF;
    uint8_t magic_lo =  MAGIC_COLOR       & 0xFF;
    for (int i = 0; i < frame_bytes; i += 2) {
        dest[i]     = magic_hi;
        dest[i + 1] = magic_lo;
    }

    // ── Composite layers bottom to top ────────────────────────────────────────
    for (size_t li = 0; li < n_layers; li++) {

        // ── Unpack layer tuple: (path_str, part_index, flip, dx, dy) ──────────
        size_t    layer_len;
        mp_obj_t *layer_fields;
        mp_obj_get_array(layer_items_raw[li], &layer_len, &layer_fields);
        if (layer_len != 5)
            mp_raise_ValueError(
                MP_ERROR_TEXT("each layer must be (path_str, part_index, flip, dx, dy)"));

        const char *path       = mp_obj_str_get_str(layer_fields[0]);
        int         part_index = mp_obj_get_int(layer_fields[1]);
        bool        flip       = mp_obj_is_true(layer_fields[2]);
        int         dx         = mp_obj_get_int(layer_fields[3]);
        int         dy         = mp_obj_get_int(layer_fields[4]);

        if (part_index < 0)
            mp_raise_ValueError(MP_ERROR_TEXT("part_index must be >= 0"));

        // ── Open file and seek to frame offset ────────────────────────────────
        FILE *f = fopen(path, "rb");
        if (!f) {
            mp_raise_msg_varg(&mp_type_OSError,
                MP_ERROR_TEXT("sprite_builder: cannot open %s"), path);
        }

        long offset = (long)part_index * frame_bytes;
        if (fseek(f, offset, SEEK_SET) != 0) {
            fclose(f);
            mp_raise_msg_varg(&mp_type_OSError,
                MP_ERROR_TEXT("sprite_builder: seek failed in %s"), path);
        }

        size_t read = fread(frame_buf, 1, frame_bytes, f);
        fclose(f);

        if ((int)read != frame_bytes) {
            mp_raise_msg_varg(&mp_type_OSError,
                MP_ERROR_TEXT("sprite_builder: short read in %s (index out of range?)"), path);
        }

        // ── Horizontal flip into flip_buf if requested ────────────────────────
        const uint8_t *src;
        if (flip) {
            for (int row = 0; row < part_h; row++) {
                for (int col = 0; col < part_w; col++) {
                    int si = (row * part_w + col)                * 2;
                    int di = (row * part_w + (part_w - 1 - col)) * 2;
                    flip_buf[di]     = frame_buf[si];
                    flip_buf[di + 1] = frame_buf[si + 1];
                }
            }
            src = flip_buf;
        } else {
            src = frame_buf;
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

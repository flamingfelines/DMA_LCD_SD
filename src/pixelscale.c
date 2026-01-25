// pixelscale.c - Fast integer upscaling for pixel art
#include "py/runtime.h"
#include "py/obj.h"

// Integer upscaling with width/height parameters
static mp_obj_t pixelscale_scale2d(size_t n_args, const mp_obj_t *args) {
    // args: src_buffer, width, height, scale
    mp_buffer_info_t src_buf;
    mp_get_buffer_raise(args[0], &src_buf, MP_BUFFER_READ);
    
    mp_int_t src_w = mp_obj_get_int(args[1]);
    mp_int_t src_h = mp_obj_get_int(args[2]);
    mp_int_t scale = mp_obj_get_int(args[3]);
    
    if (scale < 1 || scale > 16) {
        mp_raise_ValueError(MP_ERROR_TEXT("scale must be 1-16"));
    }
    
    const int bpp = 2; // RGB565
    
    mp_int_t dst_w = src_w * scale;
    mp_int_t dst_h = src_h * scale;
    mp_int_t dst_len = dst_w * dst_h * bpp;
    
    byte *src = (byte *)src_buf.buf;
    byte *dst = m_new(byte, dst_len);
    
    // Optimized 2D scaling
    for (mp_int_t y = 0; y < src_h; y++) {
        for (mp_int_t x = 0; x < src_w; x++) {
            mp_int_t src_idx = (y * src_w + x) * bpp;
            
            // Read source pixel once
            uint16_t pixel = *(uint16_t *)(src + src_idx);
            
            // Write scaled block
            for (mp_int_t dy = 0; dy < scale; dy++) {
                mp_int_t row_start = ((y * scale + dy) * dst_w + x * scale) * bpp;
                uint16_t *dst_row = (uint16_t *)(dst + row_start);
                
                // Fast fill for horizontal repetition
                for (mp_int_t dx = 0; dx < scale; dx++) {
                    dst_row[dx] = pixel;
                }
            }
        }
    }
    
    return mp_obj_new_bytearray_by_ref(dst_len, dst);
}

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pixelscale_scale2d_obj, 4, 4, pixelscale_scale2d);

static const mp_rom_map_elem_t pixelscale_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_pixelscale) },
    { MP_ROM_QSTR(MP_QSTR_scale2d), MP_ROM_PTR(&pixelscale_scale2d_obj) },
};
static MP_DEFINE_CONST_DICT(pixelscale_module_globals, pixelscale_module_globals_table);

const mp_obj_module_t pixelscale_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&pixelscale_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_pixelscale, pixelscale_module);

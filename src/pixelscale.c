#include "py/runtime.h"
#include "py/obj.h"

// Fast integer upscaling with nearest neighbor
STATIC mp_obj_t pixelscale_scale(mp_obj_t src_obj, mp_obj_t scale_obj) {
    // Get source buffer
    mp_buffer_info_t src_buf;
    mp_get_buffer_raise(src_obj, &src_buf, MP_BUFFER_READ);
    
    // Get scale factor
    mp_int_t scale = mp_obj_get_int(scale_obj);
    if (scale < 1 || scale > 16) {
        mp_raise_ValueError(MP_ERROR_TEXT("scale must be 1-16"));
    }
    
    // Assume RGB565 format (2 bytes per pixel)
    // For other formats, adjust bytes_per_pixel
    const int bytes_per_pixel = 2;
    
    // Calculate dimensions (assuming square or you pass width/height separately)
    // For this example, assume width is passed or calculated
    mp_int_t src_len = src_buf.len;
    
    // Allocate output buffer
    mp_int_t dst_len = src_len * scale * scale;
    byte *dst = m_new(byte, dst_len);
    
    byte *src = (byte *)src_buf.buf;
    mp_int_t src_pixels = src_len / bytes_per_pixel;
    
    // For actual use, you'd pass width/height
    // This is a simple 1D approach - adapt to 2D with width parameter
    for (mp_int_t y = 0; y < src_pixels; y++) {
        for (mp_int_t sy = 0; sy < scale; sy++) {
            for (mp_int_t sx = 0; sx < scale; sx++) {
                mp_int_t dst_idx = (y * scale + sy) * bytes_per_pixel;
                mp_int_t src_idx = y * bytes_per_pixel;
                
                // Copy pixel data
                for (int b = 0; b < bytes_per_pixel; b++) {
                    dst[dst_idx + b] = src[src_idx + b];
                }
            }
        }
    }
    
    return mp_obj_new_bytearray_by_ref(dst_len, dst);
}

// Better version with width/height parameters
STATIC mp_obj_t pixelscale_scale2d(size_t n_args, const mp_obj_t *args) {
    // args: src_buffer, width, height, scale
    mp_buffer_info_t src_buf;
    mp_get_buffer_raise(args[0], &src_buf, MP_BUFFER_READ);
    
    mp_int_t src_w = mp_obj_get_int(args[1]);
    mp_int_t src_h = mp_obj_get_int(args[2]);
    mp_int_t scale = mp_obj_get_int(args[3]);
    
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
                
                // Fast memcpy-like fill for horizontal repetition
                for (mp_int_t dx = 0; dx < scale; dx++) {
                    dst_row[dx] = pixel;
                }
            }
        }
    }
    
    return mp_obj_new_bytearray_by_ref(dst_len, dst);
}

STATIC MP_DEFINE_CONST_FUN_OBJ_2(pixelscale_scale_obj, pixelscale_scale);
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pixelscale_scale2d_obj, 4, 4, pixelscale_scale2d);

STATIC const mp_rom_map_elem_t pixelscale_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_pixelscale) },
    { MP_ROM_QSTR(MP_QSTR_scale), MP_ROM_PTR(&pixelscale_scale_obj) },
    { MP_ROM_QSTR(MP_QSTR_scale2d), MP_ROM_PTR(&pixelscale_scale2d_obj) },
};
STATIC MP_DEFINE_CONST_DICT(pixelscale_module_globals, pixelscale_module_globals_table);

const mp_obj_module_t pixelscale_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&pixelscale_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_pixelscale, pixelscale_module);

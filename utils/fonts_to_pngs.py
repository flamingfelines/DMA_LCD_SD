#!/usr/bin/env python
"""
Convert bitmap font files to PNG preview images.
Usage: python fonts_to_pngs.py <font_directory> <output_directory>
"""
import importlib.util
import png
import sys
import os

def render_font_to_png(font_file, output_png):
    # Load the font module
    spec = importlib.util.spec_from_file_location('font', font_file)
    if spec is None:
        raise RuntimeError(f"Could not create spec for {font_file}")
    
    font = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(font)
    
    # Font properties
    char_map = font.MAP
    height = font.HEIGHT
    max_width = font.MAX_WIDTH
    offset_width = font.OFFSET_WIDTH
    
    # Layout properties
    chars_per_row = 16
    num_chars = len(char_map)
    num_rows = (num_chars + chars_per_row - 1) // chars_per_row
    padding = 2
    
    # Image dimensions
    img_width = chars_per_row * (max_width + padding) + padding
    img_height = num_rows * (height + padding) + padding
    
    # Create blank image (white background = 0 for greyscale)
    img = [[0 for _ in range(img_width)] for _ in range(img_height)]
    
    # Reconstruct the bit string from BITMAPS
    bit_string = ''
    for byte_val in font.BITMAPS:
        bit_string += f'{byte_val:08b}'
    
    # Render each character
    for char_idx in range(num_chars):
        # Get character dimensions
        char_width = font.WIDTHS[char_idx]
        
        # Get bit offset (not byte offset!)
        offset_idx = char_idx * offset_width
        if offset_width == 2:
            bit_offset = (font.OFFSETS[offset_idx] << 8) | font.OFFSETS[offset_idx + 1]
        else:
            bit_offset = font.OFFSETS[char_idx]
        
        # Calculate position in grid
        grid_row = char_idx // chars_per_row
        grid_col = char_idx % chars_per_row
        
        # Starting position in image
        start_x = padding + grid_col * (max_width + padding)
        start_y = padding + grid_row * (height + padding)
        
        # Extract this character's bits
        char_bits = char_width * height
        char_bit_string = bit_string[bit_offset:bit_offset + char_bits]
        
        # Render character
        for y in range(height):
            for x in range(char_width):
                bit_idx = y * char_width + x
                if bit_idx < len(char_bit_string) and char_bit_string[bit_idx] == '1':
                    img_y = start_y + y
                    img_x = start_x + x
                    if img_y < img_height and img_x < img_width:
                        img[img_y][img_x] = 1
    
    # Write PNG
    with open(output_png, 'wb') as f:
        writer = png.Writer(width=img_width, height=img_height, greyscale=True, bitdepth=1)
        writer.write(f, img)
    
    print(f"✓ {os.path.basename(output_png)} ({num_chars} chars, {img_width}x{img_height})")

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print("Usage: python fonts_to_pngs.py <font_directory> <output_directory>")
        sys.exit(1)
    
    font_dir = os.path.abspath(sys.argv[1])
    output_dir = os.path.abspath(sys.argv[2])
    
    print(f"Font directory: {font_dir}")
    print(f"Output directory: {output_dir}")
    
    if not os.path.exists(font_dir):
        print(f"Error: Font directory not found: {font_dir}")
        sys.exit(1)
    
    # Create output directory
    os.makedirs(output_dir, exist_ok=True)
    
    # Get all .py files
    py_files = [f for f in os.listdir(font_dir) if f.endswith('.py')]
    print(f"Found {len(py_files)} font files\n")
    
    # Process all .py files
    for filename in sorted(py_files):
        font_path = os.path.join(font_dir, filename)
        png_name = filename.replace('.py', '.png')
        png_path = os.path.join(output_dir, png_name)
        
        try:
            render_font_to_png(font_path, png_path)
        except Exception as e:
            print(f"✗ {filename}: {e}")
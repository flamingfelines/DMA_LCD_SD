# ESP-IDF General DMA-Enabled SPI MicroPython Driver
## For ESP32-S3 Devices with ST7789 or Compatible Displays + SD Cards

> **Warning:** Not tested or coded for working with i80 LCD screens, only SPI. Only tested on XIAO ESP32-S3 with ST7789 display.

---

## Overview

This is a driver for MicroPython for devices using the `esp_lcd` SPI interface. The i80 bus code is legacy from the previous driver and non-functioning. The driver is written in C and is based on [devbis' st7789_mpy driver](https://github.com/devbis/st7789_mpy).

Russ Hughes modified the original driver to add:

- Support for esp-idf ESP_LCD Intel 8080 parallel and SPI interfaces using DMA.
- Display framebuffer enabling alpha blending for many drawing methods.
- Display rotation, hardware scrolling.
- Writing text using fonts converted from TrueType fonts.
- Drawing text using eight and 16-bit wide bitmap fonts.
- Drawing text using 26 included Hershey vector fonts.
- Drawing JPGs using TJpgDec (Tiny JPEG Decompressor R0.01d).
- Drawing PNGs using the pngle library.
- Writing PNGs from the framebuffer using PNGenc.
- Drawing and rotating polygons and filled polygons.

How I modified the original s3lcd driver:
- Creates a master SPI object instead of the LCD creating its own exclusive-use one.
- Updated SPI creation to initialize GPIO and `BUS_MASTER` flags for the ESP v5.4.2 API.
- Created `esp_sd`, a C-based driver that initiates an SD card using the general SPI bus and wraps it for use by MicroPython's existing VFS functions.
- Debug test with `tft_config` and `sd_card_config` is provided for: XIAO ESP32-S3 with ST7789 display.

---

## Pre-compiled Firmware

The firmware directory contains pre-compiled MicroPython v1.26 firmware compiled using ESP-IDF v5.4.2. The firmware includes the C driver and several frozen Python font files. See the `README.md` in the `fonts` folder for more information.

> To compile your own firmware, you will likely need to increase partition size slightly — increasing to 2.5 MB has been sufficient.

---

## Module Overview

| Module | Purpose |
|---|---|
| `esp_spi` | Shared SPI bus initialization |
| `esp_lcd` | LCD panel driver (SPI_BUS + ESPLCD) |
| `esp_sd` | SD card raw block device |
| `animation` | Sprite compositing and drawing into a framebuffer |
| `pixelscale` | Integer pixel art upscaling |

---

## `esp_spi` — SPI Bus

Manages the underlying SPI bus shared between the display and SD card. Must be initialized before either `esp_lcd` or `esp_sd`.

### `esp_spi.SPIBus(miso, mosi, sclk [, host])`

Create an SPI bus object. Only takes positional arguments.

| Parameter | Type | Description |
|---|---|---|
| `miso` | int | MISO GPIO pin number |
| `mosi` | int | MOSI GPIO pin number |
| `sclk` | int | SCLK GPIO pin number |
| `host` | int | SPI host ID (optional, defaults to `SPI2_HOST`) |

Valid GPIO range for ESP32-S3: 0–48. Valid hosts: `SPI1_HOST`, `SPI2_HOST`, `SPI3_HOST`.

```python
import esp_spi
spi = esp_spi.SPIBus(miso=8, mosi=9, sclk=7)
spi.init()
```

### `spi.init()`

Initialize the SPI bus hardware. Must be called before the bus is used by any device. Safe to call multiple times — subsequent calls are no-ops if already initialized.

### `spi.deinit()`

Release the SPI bus hardware. All devices sharing the bus must be deinitialized first.

---

## `esp_lcd` — LCD Display

### `esp_lcd.SPI_BUS(bus, spi_host, dc [, cs, ...])`

Configure the LCD's SPI I/O interface. This wraps the shared SPI bus for use with the `ESPLCD` panel driver.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `bus` | SPIBus | Yes | — | `esp_spi.SPIBus` object |
| `spi_host` | int | Yes | — | Integer matching the host used when creating the SPIBus |
| `dc` | int | Yes | — | D/C (data/command) GPIO pin number |
| `cs` | int | No | `-1` | CS GPIO pin number (positional, not keyword) |
| `spi_mode` | int | No | `0` | Traditional SPI mode (0–3) |
| `pclk` | int | No | `40000000` | Pixel clock frequency in Hz |
| `lcd_cmd_bits` | int | No | `8` | Bit-width of LCD command phase |
| `lcd_param_bits` | int | No | `8` | Bit-width of LCD parameter phase |
| `dc_low_on_data` | int | No | `0` | If set, D/C=0 means data, D/C=1 means command |
| `octal_mode` | bool | No | `False` | Transmit using octal mode (8 data lines) |
| `lsb_first` | bool | No | `False` | Transmit LSB first |
| `swap_color_bytes` | bool | No | `False` | Swap byte order of color data |

```python
import esp_lcd
lcd_bus = esp_lcd.SPI_BUS(spi, spi_host=2, dc=3, cs=44, pclk=40000000, swap_color_bytes=True)
```

---

### `esp_lcd.ESPLCD(bus, width, height [, reset, rotation, inversion_mode, dma_rows, color_space])`

Create the display object.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `bus` | SPI_BUS | Yes | — | `esp_lcd.SPI_BUS` object |
| `width` | int | Yes | — | Display width in pixels |
| `height` | int | Yes | — | Display height in pixels |
| `reset` | int | No | `-1` | Reset GPIO pin number (-1 = none) |
| `rotation` | int | No | `0` | Initial rotation (0–3) |
| `inversion_mode` | bool | No | `True` | Enable color inversion |
| `dma_rows` | int | No | `16` | Rows per DMA transfer chunk |
| `color_space` | int | No | `0` | Color space identifier |

```python
tft = esp_lcd.ESPLCD(lcd_bus, width=240, height=240, reset=2, rotation=0)
tft.init()
```

#### Built-in Rotation Tables

Default rotation tables are provided for the following display sizes:

| Display | Rotations (width, height, x_gap, y_gap, swap_xy, mirror_x, mirror_y) |
|---|---|
| 240×240 | Portrait / Landscape / Reverse Portrait / Reverse Landscape |
| 240×320 | Portrait / Landscape / Reverse Portrait / Reverse Landscape |
| 170×320 | Portrait / Landscape / Reverse Portrait / Reverse Landscape |

| Index | Rotation |
|---|---|
| 0 | Portrait (0°) |
| 1 | Landscape (90°) |
| 2 | Reverse Portrait (180°) |
| 3 | Reverse Landscape (270°) |

---

### `tft.init()`

Initialize the display panel: reset, init sequence, turn on, apply inversion and rotation. Must be called before `blit_buffer`. Also (re-)allocates the DMA transfer buffer.

### `tft.deinit()`

Free the DMA buffer and release the panel handle. Call before reinitializing the display without a hard reset.

### `tft.rotation(r)`

Set the display rotation. `r` is 0–3 (values are wrapped with `% 4`).

```python
tft.rotation(1)  # landscape
```

### `tft.inversion_mode(value)`

Enable or disable color inversion. `value` is `True` or `False`.

### `tft.blit_buffer(buf, x, y, w, h)`

Transfer a region of a framebuffer to the display over DMA. The buffer must contain at least `w * h * 2` bytes of RGB565 data. The region is clipped to the display bounds automatically.

Data is transferred in chunks of `dma_rows` rows at a time. If `swap_color_bytes` was set on the bus, byte-swapping is applied per pixel during the transfer.

| Parameter | Description |
|---|---|
| `buf` | bytearray or memoryview of RGB565 data |
| `x` | Destination x (left edge) |
| `y` | Destination y (top edge) |
| `w` | Region width in pixels |
| `h` | Region height in pixels |

```python
tft.blit_buffer(memoryview(display_buf), 0, 0, 240, 240)
```

### `tft.png_write(display_buf, filename [, x, y, w, h])`

Save the framebuffer (or a rectangular crop of it) as a PNG file using PNGenc. If the crop parameters are omitted, the entire display area is saved. The PNG is written using level-9 compression.

```python
tft.png_write(display_buf, "/sd/screenshot.png")
tft.png_write(display_buf, "/sd/crop.png", 10, 10, 100, 100)
```

---

## `esp_sd` — SD Card

> **Important:** Initialize the SD card **after** the display for stability.

### `esp_sd.SDCard(bus, cs_pin [, freq_mhz])`

Create an SD card object. Only takes positional arguments.

| Parameter | Type | Default | Description |
|---|---|---|---|
| `bus` | SPIBus | — | Initialized `esp_spi.SPIBus` object |
| `cs_pin` | int | — | CS GPIO pin number (0–48) |
| `freq_mhz` | int | `20` | SPI frequency in MHz (1–80) |

```python
import esp_sd
sd = esp_sd.SDCard(spi, 41, 20)
sd.init()
```

### `sd.init()`

Initialize the SD card over SPI. Detects block count and block size from the card's CSD register. Configures a pull-up on the MISO line after initialization (ESP-IDF 5.4.2 requirement). Safe to call only once — subsequent calls are no-ops if already initialized.

### `sd.readblocks(block_num, buf)`

Read one or more 512-byte blocks starting at `block_num` into `buf`. The number of blocks read is `len(buf) // block_size`. Used by MicroPython VFS internally.

### `sd.writeblocks(block_num, buf)`

Write one or more 512-byte blocks from `buf` starting at `block_num`. Used by MicroPython VFS internally.

### `sd.count()`

Return the total number of blocks on the card.

### `sd.ioctl(op, arg)`

VFS control interface. Supports:
- `op=4` — return block count
- `op=5` — return block size (always 512)

### `sd.deinit()`

Remove the SPI device handle and free the card structure.

### Mounting with VFS

Once initialized, pass the SD object directly to MicroPython's VFS:

```python
import vfs
vfs.mount(sd, '/sd')

# Standard file I/O now works
with open('/sd/hello.txt', 'w') as f:
    f.write('hello')
```

---

## `animation` — Sprite Compositor

Manages up to 16 sprite "slots" and composites them into a flat RGB565 framebuffer. Transparency is handled via a magic color key. All drawing targets a Python `bytearray` that you manage; hardware blitting is done separately via `tft.blit_buffer`.

**Pipeline per frame:**
```
fill_background(display_buf, bg_data)
→ draw_all(display_buf)
→ tft.blit_buffer(memoryview(display_buf), 0, 0, w, h)
```

**Magic transparency color:** `RGB(231, 154, 99)` — RGB565 value `58572` (`0xE49A63`). Pixels of this exact color in any sprite buffer are treated as transparent and skipped during compositing.

---

### Setup

#### `animation.set_display_size(w, h)`

Set the display dimensions. Must be called once at startup before any other `animation` calls. Also call this if the display is rotated to a different width/height.

```python
animation.set_display_size(240, 240)
```

---

### Scene Management

#### `animation.clear_slots()`

Disable and clear all 16 slots. Call this on every scene transition before setting up the new scene's slots. Nulls all slot buffers and resets opacity, clip, and enabled state.

```python
animation.clear_slots()
```

---

### Slot Management

Slots are drawn in ascending index order — slot 0 is the bottommost layer, slot 15 is topmost. Indices 0–15 are available.

---

#### `animation.set_slot(index, buf, x, y, w, h)`

Register a new sprite in a slot and enable it. Use this for initial setup or whenever the sprite's pixel dimensions change.

| Parameter | Description |
|---|---|
| `index` | Slot number (0–15) |
| `buf` | bytearray of RGB565 pixel data |
| `x` | X position on screen |
| `y` | Y position on screen |
| `w` | Sprite width in pixels |
| `h` | Sprite height in pixels |

Resets opacity to 255 and clears any active clipping.

```python
animation.set_slot(0, background_data, 0, 0, 240, 240)
animation.set_slot(1, pet_frame, pet_x, pet_y, 66, 66)
```

---

#### `animation.update_slot(index, buf, x, y)`

Update a slot's buffer and position simultaneously. Width and height are unchanged — use `set_slot` if those need to change. Best for sprites that change both frame and position every tick.

```python
animation.update_slot(1, pet_frame, pet_x, pet_y)
```

---

#### `animation.update_slot_buf(index, buf)`

Update only a slot's frame buffer. Position is unchanged. Best for stationary sprites that cycle through animation frames.

```python
animation.update_slot_buf(3, menu_frames[current_menu_frame])
```

---

#### `animation.update_slot_pos(index, x, y)`

Update only a slot's screen position. Frame buffer is unchanged. Best for sprites that slide across the screen without changing frame.

```python
animation.update_slot_pos(4, enemy_x, enemy_y)
```

---

#### `animation.enable_slot(index, enabled)`

Show or hide a slot without clearing its data. Re-enabling restores the last registered buffer and position.

```python
animation.enable_slot(5, fruit_present)   # True = show, False = hide
```

---

#### `animation.set_slot_opacity(index, opacity)`

Set the compositing opacity for a slot. Applied per-pixel during `draw_all`.

| Value | Effect |
|---|---|
| `255` | Fully opaque — fast path, direct pixel copy |
| `1–254` | Alpha-blended with the background using RGB565 lerp |
| `0` | Completely invisible — pixel is skipped entirely |

The blend formula unpacks RGB565 channels, linearly interpolates between source and destination, and repacks. Note that RGB565 has less precision than full 8-bit blending.

```python
animation.set_slot_opacity(2, 128)   # 50% transparent
animation.set_slot_opacity(2, 255)   # fully opaque
```

---

#### `animation.set_slot_clip(index, clip_x, clip_x_dir, clip_y, clip_y_dir)`

Apply axis-aligned pixel clipping to a slot. Clipping operates in screen coordinates and is applied during `draw_all`. Pass `0` for a clip value to disable that axis.

| Parameter | Type | Description |
|---|---|---|
| `index` | int | Slot number (0–15) |
| `clip_x` | int | Horizontal cutoff in screen pixels (0 = disabled) |
| `clip_x_dir` | str | `"after"` to hide pixels at x ≥ clip_x; `"before"` to hide pixels at x < clip_x |
| `clip_y` | int | Vertical cutoff in screen pixels (0 = disabled) |
| `clip_y_dir` | str | `"after"` to hide pixels at y ≥ clip_y; `"before"` to hide pixels at y < clip_y |

```python
# Hide everything at or below the waterline (clip bottom of sprite)
animation.set_slot_clip(6, 0, "after", water_surface_y, "after")

# Hide everything above a reveal line (wipe-in from top)
animation.set_slot_clip(6, 0, "after", reveal_y, "before")

# Clip the right half of a slot horizontally
animation.set_slot_clip(6, 120, "after", 0, "after")

# Disable all clipping
animation.set_slot_clip(6, 0, "after", 0, "after")
```

---

#### `animation.recolor_slot(index, color)`

Replace every non-transparent pixel in a slot's buffer with `color` (RGB565). The magic transparency color is preserved. Modifies the buffer in place permanently — use a copy if the original colors are needed again.

Useful for hit-flash effects (flash white), silhouettes, or team-color tinting.

```python
WHITE = 0xFFFF
animation.recolor_slot(1, WHITE)   # flash sprite white on hit
```

---

### Compositing

#### `animation.fill_background(display_buf, bg_data)`

Fast `memcpy` of `bg_data` into `display_buf`. Call this every frame before `draw_all` to reset the buffer to the background image. Faster than placing the background in slot 0 because it skips the per-pixel transparency check entirely.

```python
animation.fill_background(display_buf, background_data)
```

---

#### `animation.draw_all(display_buf)`

Composite all enabled, non-null slots onto `display_buf` in ascending slot order (slot 0 = bottom). For each sprite pixel, the magic color is skipped; all others are written with opacity blending applied.

```python
animation.draw_all(display_buf)
```

---

### Buffer Utilities

#### `animation.flip_buf_horizontal(src, dst, w, h)`

Mirror a sprite buffer horizontally (left↔right) into `dst`. Use at load time to pre-compute flipped versions so you can swap frames with `update_slot_buf` instead of re-flipping every frame.

```python
pet_flipped = bytearray(len(pet_frame))
animation.flip_buf_horizontal(pet_frame, pet_flipped, 66, 66)
```

---

#### `animation.flip_buf_vertical(src, dst, w, h)`

Mirror a sprite buffer vertically (top↔bottom) into `dst`. Use at load time to pre-compute upside-down frames.

```python
chomper_flipped = bytearray(len(chomper_frame))
animation.flip_buf_vertical(chomper_frame, chomper_flipped, 96, 64)
```

---

### Drawing Functions

These functions draw directly into a framebuffer bytearray without going through the slot system. Useful for HUD elements, debug overlays, or any content that doesn't benefit from the slot compositor.

---

#### `animation.fill_rect(display_buf, x, y, w, h, color)`

Fill a solid rectangle into `display_buf`. Automatically clipped to display bounds.

| Parameter | Description |
|---|---|
| `display_buf` | Writable bytearray (RGB565 framebuffer) |
| `x` | Left edge |
| `y` | Top edge |
| `w` | Width in pixels |
| `h` | Height in pixels |
| `color` | RGB565 fill color (16-bit integer) |

```python
BLACK = 0x0000
animation.fill_rect(display_buf, 10, 10, 100, 50, BLACK)
```

---

#### `animation.scroll(display_buf, dx, dy [, fill_color])`

Scroll the entire framebuffer by `(dx, dy)` pixels using software. The vacated region is filled with `fill_color` (defaults to `0`, black). Iteration order is chosen automatically to avoid read-before-write overlap.

| Parameter | Description |
|---|---|
| `dx` | Pixels to scroll horizontally (positive = right, negative = left) |
| `dy` | Pixels to scroll vertically (positive = down, negative = up) |
| `fill_color` | RGB565 color for newly exposed pixels (optional, default `0`) |

```python
animation.scroll(display_buf, 0, -2)          # scroll up 2 pixels, black fill
animation.scroll(display_buf, 4, 0, 0xFFFF)   # scroll right 4 pixels, white fill
```

---

#### `animation.write(font, text, x, y, fg, display_buf [, bg])`

Draw proportional or monospace TrueType-derived bitmap text into `display_buf`. Supports UTF-8 encoded strings and integer character codes. The font module must have `BPP`, `HEIGHT`, `OFFSET_WIDTH`, `WIDTHS`, `OFFSETS`, `BITMAPS`, and `MAP` keys.

| Parameter | Description |
|---|---|
| `font` | Font module object |
| `text` | String (UTF-8) or integer character code |
| `x` | Left edge of the text |
| `y` | Top edge of the text |
| `fg` | Foreground color (RGB565) |
| `display_buf` | Writable bytearray (RGB565 framebuffer) |
| `bg` | Background color (RGB565), optional — defaults to `-1` (transparent) |

```python
import vga2_bold_16x32 as font
WHITE = 0xFFFF
animation.write(font, "Score: 100", 10, 5, WHITE, display_buf)
animation.write(font, "Score: 100", 10, 5, WHITE, display_buf, 0x0000)  # black bg
```

---

#### `animation.text(font, text, x, y, fg, display_buf [, bg])`

Draw fixed-width bitmap font text into `display_buf`. The font module must have `WIDTH`, `HEIGHT`, `FIRST`, `LAST`, and `FONT` keys (standard MicroPython fixed-width bitmap font format). Accepts integer character codes or strings.

| Parameter | Description |
|---|---|
| `font` | Fixed-width font module object |
| `text` | String or integer character code |
| `x` | Left edge of the text |
| `y` | Top edge of the text |
| `fg` | Foreground color (RGB565) |
| `display_buf` | Writable bytearray (RGB565 framebuffer) |
| `bg` | Background color (RGB565), optional — defaults to `-1` (transparent) |

```python
import font6x8
animation.text(font6x8, "Hello!", 0, 0, 0xFFFF, display_buf)
```

---

### Typical Frame Loop

```python
# ── init (once) ───────────────────────────────────────────────────────────
animation.set_display_size(240, 240)

# ── scene setup (once per scene transition) ───────────────────────────────
animation.clear_slots()
animation.set_slot(1, pet_frame,  pet_x,  pet_y,  66, 66)
animation.set_slot(2, accessory_frame,  accessory_x,  accessory_y,  66, 66)
animation.set_slot(3, menu_frame,  menu_x,  0,       24, 24)
animation.set_slot(4, fruit_frame, fruit_x, fruit_y, 16, 16)
animation.enable_slot(4, False)          # hidden until fruit spawns
animation.set_slot_opacity(2, 180)       # accessory is slightly transparent

# ── each frame ────────────────────────────────────────────────────────────
animation.update_slot(1, pet_frame, pet_x, pet_y)    # frame + pos changed
animation.update_slot(2, accessory_frame, accessory_x, accessory_y)    # frame + pos changed
animation.update_slot_buf(3, menu_frames[menu_idx])     # pos fixed, frame changed
animation.enable_slot(4, fruit_present)                 # show/hide as needed

animation.fill_background(display_buf, background_data)
animation.draw_all(display_buf)
tft.blit_buffer(memoryview(display_buf), 0, 0, 240, 240)
```

---

### Notes

- The magic transparency color is hardcoded as `RGB(231, 154, 99)` — RGB565 packed value `58572` (`0xE49A63`). Sprite pixels of this exact color are always skipped during compositing.
- Slot 0 is reserved by convention for the background, but is not enforced. Using `fill_background` instead of slot 0 is recommended — it skips per-pixel transparency checks on the background.
- Slots retain their data after `enable_slot(index, False)`. Re-enabling restores the last registered buffer and position.
- `clear_slots()` nulls all buffers and disables all slots. Always call it before setting up a new scene.
- `recolor_slot` modifies the buffer permanently in place. Keep a copy of the original if you need to restore colors.
- `set_slot_clip` evaluates clipping in screen coordinates, not sprite-local coordinates.

---

## `pixelscale` — Pixel Art Upscaling

Fast integer-factor upscaling for RGB565 sprite data. Each source pixel is expanded into an `n × n` block in the output. Useful for rendering small pixel-art sprites at 2× or 3× on higher-resolution displays.

### `pixelscale.scale2d(src_buffer, width, height, scale)`

Scale a 2D RGB565 image by an integer factor.

| Parameter | Type | Description |
|---|---|---|
| `src_buffer` | bytearray | Source RGB565 pixel data |
| `width` | int | Source image width in pixels |
| `height` | int | Source image height in pixels |
| `scale` | int | Integer scale factor (1–16) |

Returns a **new bytearray** of size `(width × scale) × (height × scale) × 2` bytes. The source buffer is not modified.

Scale factor must be between 1 and 16 inclusive; values outside this range raise `ValueError`.

```python
import pixelscale

# Scale a 32×32 sprite to 64×64 (2×)
big = pixelscale.scale2d(sprite_data, 32, 32, 2)

# Scale a 16×16 icon to 48×48 (3×) and put it in a slot
scaled = pixelscale.scale2d(icon_data, 16, 16, 3)
animation.set_slot(5, scaled, icon_x, icon_y, 48, 48)
```

The scaling is performed using a single read per source pixel with a fast inner loop for horizontal repetition, making it efficient for frame-by-frame sprite animation.

---

## Building the Firmware

See the MicroPython [Getting Started](https://docs.micropython.org/en/latest/develop/gettingstarted.html) page for detailed information.

### Clone the Repositories

```bash
git clone git@github.com:micropython/micropython.git
git clone https://github.com/russhughes/s3lcd.git
```

Compile the cross compiler if you haven't already:

```bash
make -C micropython/mpy-cross
```

### ESP32 MicroPython 1.20 and Later

> Use full path addresses for files if something is not working.

```bash
cd micropython/ports/esp32

make \
    BOARD=ESP32_GENERIC_S3 \
    USER_C_MODULES=../../../../s3lcd/src/micropython.cmake \
    FROZEN_MANIFEST=../../../../s3lcd/manifest.py \
    clean submodules all
```

Erase and deploy:

```bash
make \
    BOARD=ESP32_GENERIC_S3 \
    USER_C_MODULES=../../../../s3lcd/src/micropython.cmake \
    FROZEN_MANIFEST=../../../../s3lcd/manifest.py \
    erase deploy
```

---

## Thanks

- [devbis](https://github.com/devbis) — original driver this is based on.
- [hklang10](https://github.com/hklang10) — for the updated `mp_raise_ValueError()`.
- [aleggon](https://github.com/aleggon) — correct offsets for 240×240 displays and STM32 compile fixes.
- [russhughes](https://github.com/russhughes) — for providing such a robust LCD driver.

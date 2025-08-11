#ifndef __s3lcd_spi_bus_H__
#define __s3lcd_spi_bus_H__
#include "mphalport.h"
#include "py/obj.h"
#include "esp_lcd_panel_io.h"

// spi Configuration for shared machine.SPI bus
typedef struct _s3lcd_spi_bus_obj_t {
    mp_obj_base_t base;                     // base class
    char *name;                             // name of the display
    
    // machine.SPI object reference (replaces individual SPI pin config)
    mp_obj_t machine_spi_obj;               // Reference to machine.SPI object
    
    // Display-specific GPIO pins
    int cs_gpio_num;                        // GPIO used for CS line
    int dc_gpio_num;                        // GPIO used to select the D/C line, set this to -1 if the D/C line not controlled by manually pulling high/low GPIO
    
    // SPI and LCD configuration
    int spi_mode;                           // Traditional SPI mode (0~3)
    unsigned int pclk_hz;                   // Frequency of pixel clock
    int lcd_cmd_bits;                       // Bit-width of LCD command
    int lcd_param_bits;                     // Bit-width of LCD parameter
    
    // ESP-LCD panel I/O handle
    esp_lcd_panel_io_handle_t io_handle;    // ESP-LCD panel I/O handle
    
    struct {                                // Extra flags to fine-tune the SPI device
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
        unsigned int dc_as_cmd_phase: 1;    // D/C line value is encoded into SPI transaction command phase
#endif
        unsigned int dc_low_on_data: 1;     // If this flag is enabled, DC line = 0 means transfer data, DC line = 1 means transfer command; vice versa
        unsigned int octal_mode: 1;         // transmit with octal mode (8 data lines), this mode is used to simulate Intel 8080 timing
        unsigned int lsb_first: 1;          // transmit LSB bit first
        unsigned int swap_color_bytes:1;    // Swap color bytes in 16-bit color mode
    } flags;
} s3lcd_spi_bus_obj_t;

extern const mp_obj_type_t s3lcd_spi_bus_type;

#endif /* __s3lcd_spi_bus_H__ */

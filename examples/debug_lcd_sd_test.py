#Current Pinouts for a XIAO ESP32-S3 and a ST7789 screen with a standard micro sd card module and a 32GB micro sd card. 
#Should theoretically work with other basic SPI displays and micro SD cards. 
#Must have a file named "Test.rgb565" on micro sd card if you want to test the interaction functionality. 

import esp_spi
import esp_sd
import s3lcd_spi_bus
import s3lcd
import vfs
from machine import Pin, PWM

MISO_PIN = 8
MOSI_PIN = 9
SCLK_PIN = 7
LCD_DC_PIN = 2
LCD_CS_PIN = 1
SD_CS_PIN = 44

SPI_HOST = 1
ROTATION = 2

SCREEN_WIDTH = 240
SCREEN_HEIGHT = 240

sd_card = False
display = False
buffer = bytearray((SCREEN_WIDTH * SCREEN_HEIGHT * 2))

# Setup PWM backlight control
backlight_pwm = PWM(Pin(3, Pin.OUT, value=0))
def set_backlight(brightness):
    """Set backlight brightness (0.0 to 1.0)"""
    duty = int(brightness * 65535)
    backlight_pwm.duty_u16(duty)

def init_spi():
    # 1. Create and initialize the SPI bus
    spi = esp_spi.SPIBus(MISO_PIN, MOSI_PIN, SCLK_PIN)
    try:
        spi.init()
        print(f"SPI initiated")
    except:
        print(f"SPI failed to initiate")

def init_sd():
    global sd_card
    sd_card = esp_sd.SDCard(spi, int(SD_CS_PIN))  # No keywords
    print("SDCard created successfully!")
    # Try initiating sd_card. 
    sd_card.init()
    print("SDCard initiated!")

def mount_sd():
    # Mount as filesystem
    vfs.mount(sd_card, '/sd')
    print("SD card mounted at /sd")

def load_rgb565_image(filename):
    """Load RGB565 image data from SD card"""
    try:
        vfs.mount(sd_card, '/sd')
        print("SD card mounted at /sd")
        with open(f'/sd/{filename}', 'rb') as f:
            data = f.read()
        print(f"✓ Loaded {filename} ({len(data)} bytes)")
        return data
    except Exception as e:
        print(f"✗ Error loading {filename}: {e}")
        return None
    vfs.unmount(sd_card, '/sd')

def setup_display():
    global display
    try:
        lcd_bus = s3lcd_spi_bus.SPI_BUS(spi, SPI_HOST, LCD_DC_PIN, LCD_CS_PIN )
        print(f"LCD_BUS initiated")
    except:
        print(f"LCD_BUS failed to initiate")
        
    try:
        display = s3lcd.ESPLCD(lcd_bus, SCREEN_WIDTH, SCREEN_HEIGHT, rotation=ROTATION, dma_rows = SCREEN_WIDTH)
        print(f"Display created")
    except:
        print(f"Display failed to create")
        
def debug_display():
    global display
    # Basic display tests
    display.init()
    print("✅ Display initialized")
    
    #Test 1: Fill screen with colors
    display.fill(s3lcd.BLUE)  # Red
    try:
        display.show()
        print("✅ Red screen test")
        #display.print()
    except: print("FUCK")
    
def debug_display_2():
    display.fill(0x07E0)  # Green
    display.show()
    print("✅ Green screen test")
    
    display.fill(0x001F)  # Blue
    display.show()
    print("✅ Blue screen test")
    
    display.fill(0x0000)  # Black
    display.show()
    print("✅ Black screen test")
    
    # Test 2: Draw some pixels
    for x in range(0, 240, 10):
        for y in range(0, 240, 10):
            display.pixel(x, y, 0xFFFF)  # White pixels
    display.show()
    print("✅ Pixel grid test")
    
    # Test 3: Draw lines
    for i in range(0, 240, 20):
        display.line(0, i, 239, i, 0xFFE0)  # Yellow horizontal lines
        display.line(i, 0, i, 239, 0xF81F)  # Magenta vertical lines
    display.show()
    print("✅ Line drawing test")
    
    print("🎉 Display test completed!")

def debug_sd():
    global sd_card
    # Test 1: Basic info
    print("SD card object:", sd_card)

    # Test 2: Get card info
    try:
        block_count = sd_card.count()
        print(f"SD card has {block_count} blocks")
        print(f"Card size: {block_count * 512 / (1024*1024):.1f} MB")
    except Exception as e:
        print("Count error:", e)
        
    try:
        # Read the first block (boot sector/partition table)
        buffer = bytearray(512)
        sd_card.readblocks(0, buffer)
        print("Successfully read block 0!")
        print("First 32 bytes:", buffer[:32].hex())
        
        # Check if it looks like a valid boot sector
        if buffer[510] == 0x55 and buffer[511] == 0xAA:
            print("Valid boot sector signature found!")
        else:
            print("Boot sector signature:", hex(buffer[510]), hex(buffer[511]))
            
    except Exception as e:
        print("Read error:", e)
        
    try:
        sector_count = sd_card.ioctl(4, 0)  # BP_IOCTL_SEC_COUNT
        sector_size = sd_card.ioctl(5, 0)   # BP_IOCTL_SEC_SIZE
        print(f"ioctl - Sectors: {sector_count}, Size: {sector_size}")
    except Exception as e:
        print("ioctl error:", e)
        
init_spi()
setup_display()
init_sd()
debug_display()
background_data = load_rgb565_image("Test.rgb565")
buffer[:] = background_data
display.blit_buffer(buffer, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT)
display.show()
debug_display_2()

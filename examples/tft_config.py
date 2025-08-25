from machine import Pin, PWM
import esp_spi
import esp_sd
import s3lcd_spi_bus
import s3lcd
import vfs
import gc
import utime

# Global state
config = {
    "spi" : None,
    "display" : None,
    "sd_card" : None, 
    "sd_mounted" : False,
    "spi_initiated" : False,
    "backlight_on" : True,
    }


MISO_PIN = 8
MOSI_PIN = 9
SCLK_PIN = 7
LCD_DC_PIN = 2
LCD_CS_PIN = 1
SD_CS_PIN = 44

SPI_HOST = 1

SCREEN_WIDTH = 240
SCREEN_HEIGHT = 240

# Setup PWM backlight control
backlight_pwm = PWM(Pin(3, Pin.OUT, value=0))

def set_backlight(brightness):
    """Set backlight brightness (0.0 to 1.0)"""
    duty = int(brightness * 65535)
    backlight_pwm.duty_u16(duty)

set_backlight(0)

def fade_backlight_on(duration_ms=200):
    """Smooth fade from off to full brightness"""
    steps = 50
    step_delay = duration_ms // steps
    
    for i in range(steps + 1):
        brightness = i / steps
        set_backlight(brightness)
        utime.sleep_ms(step_delay)
    config["backlight_on"] = True
        
def fade_backlight_off(duration_ms=200):
    """Smooth fade from full to off"""
    steps = 30
    step_delay = duration_ms // steps
    
    for i in range(steps, -1, -1):
        brightness = i / steps
        set_backlight(brightness)
        utime.sleep_ms(step_delay)
    config["backlight_on"] = False
        
'''===========================================
        DISPLAY AND SD CARD CONTROL'''

def init_spi():
    if not config.get("spi_initiated"):
        try:
        # 1. Create and initialize the SPI bus
            spi = esp_spi.SPIBus(MISO_PIN, MOSI_PIN, SCLK_PIN)
            config["spi"] = spi
            spi.init()
            config["spi_initiated"] = True
            print(f"SPI initiated")
        except:
            print(f"SPI failed to initiate")

def setup_display():
    if not config.get("display"):
        try:
            lcd_bus = s3lcd_spi_bus.SPI_BUS(config.get("spi"), SPI_HOST, LCD_DC_PIN, LCD_CS_PIN, pclk = 80000000)
            print(f"LCD_BUS initiated")
        except:
            print(f"LCD_BUS failed to initiate")
            
        try:
            display = s3lcd.ESPLCD(lcd_bus, SCREEN_WIDTH, SCREEN_HEIGHT, rotation=2, dma_rows = 240)
            print(f"Display created")
            config["display"] = display
            display.init()
        except:
            print(f"Display failed to create")

def init_sd(): #Must be called after setup_display. 
    if not config.get("sd_card"):
        sd_card = esp_sd.SDCard(config.get("spi"), int(SD_CS_PIN))  # No keywords
        print("SDCard created successfully!")
        # Try initiating sd_card.
        config["sd_card"] = sd_card
        sd_card.init()
        print("SDCard initiated!")

def mount_sd():
    sd_card = config.get("sd_card")
    if not config.get("sd_mounted"):
        vfs.mount(sd_card, '/sd')
        config["sd_mounted"] = True
    
def unmount_sd():
    sd_card = config.get("sd_card")
    if config.get("sd_mounted"):
        vfs.umount('/sd')
        config["sd_mounted"] = False
    
def load_rgb565_image(filename, more_data = False):
    """Load RGB565 image data from SD card"""
    try:
        mount_sd()
        with open(f'/sd/{filename}', 'rb') as f:
            data = f.read()
        print(f"✓ Loaded {filename} ({len(data)} bytes)")
        return data
    except Exception as e:
        print(f"✗ Error loading {filename}: {e}")
        return None
    if not more_data:
        unmount_sd()

def blit_image(data, x=0, y=0):
    display = config["display"]
    """Display an RGB565 image (convenience function)"""
    if data:
        display.blit_buffer(memoryview(data), x, y, SCREEN_WIDTH, SCREEN_HEIGHT)
        return True
    return False

def show_display():
    display = config["display"]
    if display:
        display.show()

def first_load():
    init_spi()
    setup_display()
    init_sd()

first_load()

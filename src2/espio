#include "py/obj.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "espio";

static spi_host_device_t spi_host = SPI2_HOST; // Default hardware SPI2
static spi_bus_config_t buscfg;

// ========= Enable pull-up on MISO =========
static void enable_miso_pullup(int miso_gpio) {
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << miso_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "Enabled pull-up on GPIO%d (MISO)", miso_gpio);
}

// ========= espio.bus_init(miso, mosi, sck, dma) =========
STATIC mp_obj_t espio_bus_init(size_t n_args, const mp_obj_t *args) {
    int miso = mp_obj_get_int(args[0]);
    int mosi = mp_obj_get_int(args[1]);
    int sck  = mp_obj_get_int(args[2]);
    int dma_chan = mp_obj_get_int(args[3]);

    buscfg.miso_io_num = miso;
    buscfg.mosi_io_num = mosi;
    buscfg.sclk_io_num = sck;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = 240*240*2; // for full 240x240 RGB565 frame

    esp_err_t ret = spi_bus_initialize(spi_host, &buscfg, dma_chan);
    if (ret != ESP_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("SPI bus init failed"));
    }

    // Apply pull-up to MISO after init
    enable_miso_pullup(miso);

    ESP_LOGI(TAG, "SPI bus initialized (SCK=%d, MOSI=%d, MISO=%d, DMA=%d)", sck, mosi, miso, dma_chan);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espio_bus_init_obj, 4, 4, espio_bus_init);

// ========= Module globals =========
STATIC const mp_rom_map_elem_t espio_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_bus_init), MP_ROM_PTR(&espio_bus_init_obj) },
};
STATIC MP_DEFINE_CONST_DICT(espio_module_globals, espio_module_globals_table);

const mp_obj_module_t mp_module_espio = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&espio_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_espio, mp_module_espio);

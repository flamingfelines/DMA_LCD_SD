// espio.h
#ifndef ESPIO_H
#define ESPIO_H

#include "driver/spi_master.h"

// Expose the global host + bus config
extern spi_host_device_t spi_host;
extern spi_bus_config_t buscfg;

// Initialize the SPI bus (DMA enabled, with MISO pull-up)
void espio_bus_init(int miso, int mosi, int sck, int dma_chan);

#endif // ESPIO_H

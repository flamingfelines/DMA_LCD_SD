# Create an INTERFACE library for our C module.
add_library(usermod_dma_lcd_sd INTERFACE)
# Add our source files to the lib
target_sources(usermod_dma_lcd_sd INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/s3lcd.c
    ${CMAKE_CURRENT_LIST_DIR}/s3lcd_i80_bus.c
    ${CMAKE_CURRENT_LIST_DIR}/s3lcd_spi_bus.c
    ${CMAKE_CURRENT_LIST_DIR}/esp_spi.c
    ${CMAKE_CURRENT_LIST_DIR}/esp_sd.c
    ${CMAKE_CURRENT_LIST_DIR}/mpfile.c
    ${CMAKE_CURRENT_LIST_DIR}/jpg/tjpgd565.c
    ${CMAKE_CURRENT_LIST_DIR}/png/pngle.c
    ${CMAKE_CURRENT_LIST_DIR}/png/miniz.c
    ${CMAKE_CURRENT_LIST_DIR}/pngenc/adler32.c
    ${CMAKE_CURRENT_LIST_DIR}/pngenc/crc32.c
    ${CMAKE_CURRENT_LIST_DIR}/pngenc/deflate.c
    ${CMAKE_CURRENT_LIST_DIR}/pngenc/pngenc.c
    ${CMAKE_CURRENT_LIST_DIR}/pngenc/trees.c
    ${CMAKE_CURRENT_LIST_DIR}/pngenc/zutil.c
)
# Add the current directory as an include directory.
target_include_directories(usermod_dma_lcd_sd INTERFACE
    ${MICROPY_DIR}
    ${MICROPY_DIR}/py
    ${MICROPY_PORT_DIR}
    ${IDF_PATH}/components/esp_lcd/include/
    ${IDF_PATH}/components/hal/include/
    ${IDF_PATH}/components/soc/include/
    ${IDF_PATH}/components/esp_driver_spi/include/
    ${IDF_PATH}/components/esp_driver_gpio/include/
    ${IDF_PATH}/components/esp_driver_i2s/include/
    ${IDF_PATH}/components/sdmmc/include/
    ${IDF_PATH}/components/fatfs/vfs/
    ${CMAKE_CURRENT_LIST_DIR}
)
# Ensure fatfs component is available for SD card support
list(APPEND MICROPY_DEF_BOARD
    MICROPY_HW_ENABLE_SDCARD=1
)
# Force include fatfs component for SD card support
if(NOT "fatfs" IN_LIST BUILD_COMPONENTS)
    list(APPEND EXTRA_COMPONENT_DIRS "$ENV{IDF_PATH}/components/fatfs")
endif()

# Ensure SD card related components are included
list(APPEND BUILD_COMPONENTS fatfs sdmmc)
# Link our INTERFACE library to the usermod target.
target_link_libraries(usermod INTERFACE usermod_dma_lcd_sd)

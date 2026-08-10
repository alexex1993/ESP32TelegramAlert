FILE(GLOB app_sources ${CMAKE_SOURCE_DIR}/src/*.c)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    REQUIRES
        esp_app_format
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        lwip
        mbedtls
        esp-tls
        cjson
        driver
        esp_lcd
        esp_timer
        log
        led_strip
        fatfs
        sdmmc
        esp_driver_sdspi
)

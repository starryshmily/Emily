file(GLOB_RECURSE srcs *.c
                    spi_lcd_touch_example_main.c
                    wifi_scanner.c
                    camera_client.c
                    jpeg_decoder.c
                    c3_uart.c
                    ui/*.c
                    ui/ui_wifi.c
                    custom/gxhtc3.c
                    custom/myi2c.c
                    custom/*.c
                    generated/*.c
                    generated/guider_customer_fonts/*.c
                    generated/guider_fonts/*.c
                    generated/images/*.c
                    )

set(include_dirs
    .
    ui
    ui/fonts
    custom
    generated
    generated/guider_customer_fonts
    generated/guider_fonts
    generated/images
    )

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES
        lvgl
        esp_http_client
        esp_http_server
        json
        nvs_flash
        esp_wifi
    )

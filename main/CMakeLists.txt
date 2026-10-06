# WiFi and Pushover credentials live in secrets.h, which is never committed.
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/secrets.h")
    message(FATAL_ERROR
        "main/secrets.h is missing. Create it from the template and fill in "
        "your WiFi and Pushover credentials:\n"
        "    cp main/secrets.h.example main/secrets.h")
endif()

idf_component_register(
    SRCS "main.cpp" "board_power.cpp" "display.cpp" "ssd1681.cpp" "ui.cpp" "render.cpp" "fonts_data.cpp"
         "wifi_connect.cpp" "pushover_client.cpp" "buzzer.cpp" "buttons.cpp"
         "button_input.cpp" "audio.cpp"
    INCLUDE_DIRS "."
    REQUIRES codec_board esp_driver_spi esp_driver_gpio
             nvs_flash esp_wifi esp_netif esp_event esp_http_client
             esp_websocket_client esp_timer mbedtls driver heap json
             esp_hw_support
    )

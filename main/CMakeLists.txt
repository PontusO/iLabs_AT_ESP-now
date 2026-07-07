# Component dependencies differ between ESP-IDF (ESP32-C3/C6) and
# ESP8266_RTOS_SDK (ESP8285/ESP8266): on the 8266 SDK the UART driver,
# WiFi, ESP-NOW and esp_timer all live in the "esp8266" component and
# esp_netif is provided by "tcpip_adapter".
if(IDF_TARGET STREQUAL "esp8266")
    set(en_requires esp8266 tcpip_adapter esp_event nvs_flash)
else()
    set(en_requires driver esp_wifi esp_netif esp_event esp_timer nvs_flash)
endif()

idf_component_register(
    SRCS
        "main.c"
        "at_uart.c"
        "at_parser.c"
        "en_core.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        ${en_requires}
)

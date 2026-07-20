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

# ESP8266_RTOS_SDK v3.4 bug workaround: the pthread component force-links the
# wrong anchor symbol for its condition-variable implementation - its
# CMakeLists uses "-u pthread_include_pthread_cond_impl", but pthread_cond_var.c
# actually exports "pthread_include_pthread_cond_var_impl". As a result the cond
# object is never pulled in ahead of libstdc++, and the C++ static-init guard
# (guard.o) fails to resolve pthread_cond_init/wait/broadcast at link time.
# Force the correctly-named anchor so the firmware links. (ESP-IDF C3/C6 targets
# provide these via newlib and need no such flag.)
if(IDF_TARGET STREQUAL "esp8266")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u pthread_include_pthread_cond_var_impl")
endif()

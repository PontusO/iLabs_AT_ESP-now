# Shared AT-core: UART transport + subsystem-agnostic parser engine.
# Reused by every AT personality (ESP-NOW today, Matter next).
#
# The UART driver lives in different components between ESP-IDF (C3/C6:
# "driver") and ESP8266_RTOS_SDK (ESP8285/ESP8266: "esp8266").
if(IDF_TARGET STREQUAL "esp8266")
    set(at_core_requires esp8266)
else()
    set(at_core_requires driver)
endif()

idf_component_register(
    SRCS
        "at_uart.c"
        "at_parser.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        ${at_core_requires}
)

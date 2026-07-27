# SPDX-FileCopyrightText: 2026 Pontus Oldberg <pontus@ilabs.se>
# SPDX-License-Identifier: MIT

# Shared AT-core: UART transport + subsystem-agnostic parser engine +
# the single radio/link owner (link_mgr). Reused by every AT personality
# (one personality per image; see the sibling firmware repos).
#
# Component names differ between ESP-IDF (C3/C6) and ESP8266_RTOS_SDK
# (ESP8285/ESP8266): the UART driver, WiFi and netif all live in the
# "esp8266"/"tcpip_adapter" components on the 8266 SDK.
if(IDF_TARGET STREQUAL "esp8266")
    set(at_core_requires esp8266 tcpip_adapter esp_event)
else()
    set(at_core_requires driver esp_wifi esp_netif esp_event)
endif()

idf_component_register(
    SRCS
        "at_uart.c"
        "at_parser.c"
        "link_mgr.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        ${at_core_requires}
)

/*
 * SPDX-FileCopyrightText: 2026 Pontus Oldberg <pontus@ilabs.se>
 * SPDX-License-Identifier: MIT
 *
 * at_core_config.h
 *
 * Shared AT-core transport configuration: the UART port, pins, baud,
 * flow control and driver buffer sizes that every AT personality uses
 * to talk to the host MCU.
 *
 * Subsystem-specific tunables (payload limits, timeouts, URC options,
 * identity strings) live in the owning app's config header, which
 * #includes this file. Splitting the transport out here is design
 * invariant C4: one shared transport config, layered subsystem config.
 *
 * Targets: ESP32-C6 and ESP32-C3 (ESP-IDF v5.x) and
 *          ESP8285/ESP8266 (ESP8266_RTOS_SDK v3.4).
 */

#pragma once

#include "sdkconfig.h"

/* True when building against ESP8266_RTOS_SDK for ESP8285/ESP8266. */
#if defined(CONFIG_IDF_TARGET_ESP8266)
#define AT_TARGET_ESP8266       1
#else
#define AT_TARGET_ESP8266       0
#endif

/* ------------------------------------------------------------------ */
/*  AT transport: UART                                                 */
/* ------------------------------------------------------------------ */

/*
 * UART port used for the AT command link to the host MCU.
 *
 * NOTE: UART0 is normally the console/log UART. On ESP32-C3/C6 the AT
 * link therefore lives on UART1, leaving boot messages and esp_log
 * output on UART0 where they cannot corrupt the AT stream. If you must
 * run AT on UART0, disable console logging
 * (CONFIG_LOG_DEFAULT_LEVEL_NONE) as well.
 *
 * On ESP8285/ESP8266 the chip's UART1 is TX-only, so the AT link MUST
 * be on UART0; sdkconfig.defaults.esp8285 moves the console/log output
 * to UART1 (TX-only on GPIO2) instead.
 */
#if AT_TARGET_ESP8266
#define AT_UART_PORT            UART_NUM_0
#else
#define AT_UART_PORT            UART_NUM_1
#endif

/* Baud rate of the AT link. */
#define AT_UART_BAUD            115200

/*
 * Hardware flow control mode for the AT UART at boot (change at runtime
 * with AT+ENFLOW).
 *   AT_UART_FLOWCTRL_NONE     - no flow control (3-wire: TX/RX/GND)
 *   AT_UART_FLOWCTRL_RTS      - RTS only  (ESP32 signals host to pause)
 *   AT_UART_FLOWCTRL_CTS      - CTS only  (host signals ESP32 to pause)
 *   AT_UART_FLOWCTRL_CTS_RTS  - full RTS/CTS flow control
 */
#define AT_UART_FLOWCTRL_NONE     0
#define AT_UART_FLOWCTRL_RTS      1
#define AT_UART_FLOWCTRL_CTS      2
#define AT_UART_FLOWCTRL_CTS_RTS  3

#define AT_UART_FLOWCTRL        AT_UART_FLOWCTRL_NONE

/*
 * RX FIFO threshold at which RTS is de-asserted when RTS flow control
 * is enabled (bytes, 0..127 on C3/C6).
 */
#define AT_UART_RTS_THRESH      100

/* ------------------------------------------------------------------ */
/*  AT UART pin assignment (per target)                                */
/*                                                                     */
/*  ESP32-C3/C6: all four pins are routed through the GPIO matrix, so  */
/*  any free GPIO works. Use -1 (AT_PIN_NC) for RTS/CTS when flow      */
/*  control is disabled or when a signal is not wired.                 */
/*                                                                     */
/*  ESP8285/ESP8266: UART0 pins are FIXED by the chip's IO mux - the   */
/*  values below are informational only. TX=GPIO1, RX=GPIO3,           */
/*  RTS=GPIO15 (MTDO), CTS=GPIO13 (MTCK). The only pin option is       */
/*  AT_UART_SWAP_IO below, which swaps UART0 onto GPIO15(TX)/          */
/*  GPIO13(RX) - this keeps the ROM's 74880-baud boot chatter off the  */
/*  AT link, but makes hardware flow control unavailable (it uses the  */
/*  same pins).                                                        */
/* ------------------------------------------------------------------ */

#define AT_PIN_NC               (-1)

#if AT_TARGET_ESP8266
#define AT_UART_TX_PIN          1       /* fixed */
#define AT_UART_RX_PIN          3       /* fixed */
#define AT_UART_RTS_PIN         15      /* fixed */
#define AT_UART_CTS_PIN         13      /* fixed */
/* Set to 1 to move UART0 to GPIO15(TX)/GPIO13(RX) via the IO swap.
 * Mutually exclusive with hardware flow control. */
#define AT_UART_SWAP_IO         0
#elif CONFIG_IDF_TARGET_ESP32C6
#define AT_UART_TX_PIN          16
#define AT_UART_RX_PIN          17
#define AT_UART_RTS_PIN         19
#define AT_UART_CTS_PIN         18
#elif CONFIG_IDF_TARGET_ESP32C3
/* AT link (UART1) wired to GPIO21(TX)/GPIO20(RX); RTS/CTS not connected.
 * These are also the C3's default UART0 console pins, so the console is moved
 * off them to GPIO18/19 in sdkconfig.defaults.esp32c3. */
#define AT_UART_TX_PIN          21
#define AT_UART_RX_PIN          20
#define AT_UART_RTS_PIN         AT_PIN_NC
#define AT_UART_CTS_PIN         AT_PIN_NC
#else
/* Fallback for other targets - adjust as required. */
#define AT_UART_TX_PIN          5
#define AT_UART_RX_PIN          4
#define AT_UART_RTS_PIN         AT_PIN_NC
#define AT_UART_CTS_PIN         AT_PIN_NC
#endif

#if AT_TARGET_ESP8266 && AT_UART_SWAP_IO && \
    (AT_UART_FLOWCTRL != AT_UART_FLOWCTRL_NONE)
#error "ESP8285: UART0 swap uses GPIO15/13 - flow control is unavailable"
#endif

/* ------------------------------------------------------------------ */
/*  Transport buffers                                                  */
/* ------------------------------------------------------------------ */

/* UART driver ring buffer sizes (bytes). */
#define AT_UART_RX_BUF_SIZE     4096
#define AT_UART_TX_BUF_SIZE     4096

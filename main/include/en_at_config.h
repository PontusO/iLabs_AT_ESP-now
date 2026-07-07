/*
 * en_at_config.h
 *
 * Global build-time configuration for the ESP-NOW AT interpreter.
 *
 * Everything hardware-related (UART port, pins, baud rate, flow control)
 * and every tunable (buffer sizes, timeouts, URC options) lives in this
 * single file so a board port only ever touches one header.
 *
 * Targets: ESP32-C6 and ESP32-C3 (ESP-IDF v5.x) and
 *          ESP8285/ESP8266 (ESP8266_RTOS_SDK v3.4).
 */

#pragma once

#include "sdkconfig.h"

/* True when building against ESP8266_RTOS_SDK for ESP8285/ESP8266. */
#if defined(CONFIG_IDF_TARGET_ESP8266)
#define EN_TARGET_ESP8266       1
#else
#define EN_TARGET_ESP8266       0
#endif

/* ------------------------------------------------------------------ */
/*  Firmware identity                                                  */
/* ------------------------------------------------------------------ */

#define EN_FW_VERSION           "1.0.0"

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
#if EN_TARGET_ESP8266
#define EN_UART_PORT            UART_NUM_0
#else
#define EN_UART_PORT            UART_NUM_1
#endif

/* Baud rate of the AT link. */
#define EN_UART_BAUD            115200

/*
 * Hardware flow control mode for the AT UART.
 *   EN_UART_FLOWCTRL_NONE     - no flow control (3-wire: TX/RX/GND)
 *   EN_UART_FLOWCTRL_RTS      - RTS only  (ESP32 signals host to pause)
 *   EN_UART_FLOWCTRL_CTS      - CTS only  (host signals ESP32 to pause)
 *   EN_UART_FLOWCTRL_CTS_RTS  - full RTS/CTS flow control
 */
#define EN_UART_FLOWCTRL_NONE     0
#define EN_UART_FLOWCTRL_RTS      1
#define EN_UART_FLOWCTRL_CTS      2
#define EN_UART_FLOWCTRL_CTS_RTS  3

#define EN_UART_FLOWCTRL        EN_UART_FLOWCTRL_NONE

/*
 * RX FIFO threshold at which RTS is de-asserted when RTS flow control
 * is enabled (bytes, 0..127 on C3/C6).
 */
#define EN_UART_RTS_THRESH      100

/* ------------------------------------------------------------------ */
/*  AT UART pin assignment (per target)                                */
/*                                                                     */
/*  ESP32-C3/C6: all four pins are routed through the GPIO matrix, so  */
/*  any free GPIO works. Use -1 (EN_PIN_NC) for RTS/CTS when flow      */
/*  control is disabled or when a signal is not wired.                 */
/*                                                                     */
/*  ESP8285/ESP8266: UART0 pins are FIXED by the chip's IO mux - the   */
/*  values below are informational only. TX=GPIO1, RX=GPIO3,           */
/*  RTS=GPIO15 (MTDO), CTS=GPIO13 (MTCK). The only pin option is       */
/*  EN_UART_SWAP_IO below, which swaps UART0 onto GPIO15(TX)/          */
/*  GPIO13(RX) - this keeps the ROM's 74880-baud boot chatter off the  */
/*  AT link, but makes hardware flow control unavailable (it uses the  */
/*  same pins).                                                        */
/* ------------------------------------------------------------------ */

#define EN_PIN_NC               (-1)

#if EN_TARGET_ESP8266
#define EN_UART_TX_PIN          1       /* fixed */
#define EN_UART_RX_PIN          3       /* fixed */
#define EN_UART_RTS_PIN         15      /* fixed */
#define EN_UART_CTS_PIN         13      /* fixed */
/* Set to 1 to move UART0 to GPIO15(TX)/GPIO13(RX) via the IO swap.
 * Mutually exclusive with hardware flow control. */
#define EN_UART_SWAP_IO         0
#elif CONFIG_IDF_TARGET_ESP32C6
#define EN_UART_TX_PIN          5
#define EN_UART_RX_PIN          4
#define EN_UART_RTS_PIN         6
#define EN_UART_CTS_PIN         7
#elif CONFIG_IDF_TARGET_ESP32C3
#define EN_UART_TX_PIN          7
#define EN_UART_RX_PIN          6
#define EN_UART_RTS_PIN         5
#define EN_UART_CTS_PIN         4
#else
/* Fallback for other targets - adjust as required. */
#define EN_UART_TX_PIN          5
#define EN_UART_RX_PIN          4
#define EN_UART_RTS_PIN         EN_PIN_NC
#define EN_UART_CTS_PIN         EN_PIN_NC
#endif

#if EN_TARGET_ESP8266 && EN_UART_SWAP_IO && \
    (EN_UART_FLOWCTRL != EN_UART_FLOWCTRL_NONE)
#error "ESP8285: UART0 swap uses GPIO15/13 - flow control is unavailable"
#endif

/* ------------------------------------------------------------------ */
/*  Buffers & limits                                                   */
/* ------------------------------------------------------------------ */

/* UART driver ring buffer sizes (bytes). */
#define EN_UART_RX_BUF_SIZE     4096
#define EN_UART_TX_BUF_SIZE     4096

/*
 * Maximum accepted AT command line length (bytes, including the
 * "AT+..." prefix). Must be large enough for the largest hex-encoded
 * AT+ENFRAGSEND payload: 2 * EN_FRAG_MAX_TOTAL + ~64 bytes overhead.
 */
#define EN_AT_LINE_MAX          (2 * EN_FRAG_MAX_TOTAL + 64)

/*
 * Maximum total payload accepted by AT+ENFRAGSEND and by the receive
 * side reassembler (bytes). Bounded by 32 fragments of
 * EN_FRAG_CHUNK bytes each (see en_core.c).
 */
#define EN_FRAG_MAX_TOTAL       4096

/* Number of concurrent fragmented-receive reassembly slots. */
#define EN_FRAG_RX_SLOTS        4

/* Depth of the receive event queue between WiFi task and RX worker. */
#define EN_RX_QUEUE_LEN         16

/* ------------------------------------------------------------------ */
/*  Timeouts                                                           */
/* ------------------------------------------------------------------ */

/* Max wait for the ESP-NOW send callback per frame (ms). */
#define EN_SEND_CB_TIMEOUT_MS   1000

/* AT+ENPEERCHECK ping/pong round-trip timeout (ms). */
#define EN_PING_TIMEOUT_MS      500

/* Fragment reassembly timeout - stale slots are dropped (ms). */
#define EN_FRAG_TIMEOUT_MS      3000

/* AT+ENSENDRAW: max wait for the host to deliver the raw bytes (ms). */
#define EN_RAW_DATA_TIMEOUT_MS  10000

/* AT+ENDISCOVER: response collection window when no argument given (ms). */
#define EN_DISCOVER_TIMEOUT_MS  1000

/* ------------------------------------------------------------------ */
/*  Behaviour options                                                  */
/* ------------------------------------------------------------------ */

/* Command echo on boot (change at runtime with ATE0 / ATE1). */
#define EN_AT_ECHO_DEFAULT      0

/* Emit +ENFRAGRECV progress URCs (both send and receive side). */
#define EN_URC_FRAG_PROGRESS    1

/* Emit "+ENREADY" once on boot so the host can sync to the slave. */
#define EN_URC_READY_ON_BOOT    1

/*
 * Boot default for answering AT+ENDISCOVER scans from other nodes
 * (no host involvement). The host can toggle this at runtime with
 * AT+ENDISCOVERABLE=0|1; a reset returns to this value.
 */
#define EN_DISCOVERY_RESPOND    1

/* Max unique devices collected per AT+ENDISCOVER scan. */
#define EN_DISCOVER_MAX         32

/*
 * Random 0..N ms delay before answering a discovery request, so a large
 * fleet does not reply in the same instant and collide. 0 disables.
 */
#define EN_DISCOVER_JITTER_MS   50

/* ------------------------------------------------------------------ */
/*  Task tuning                                                        */
/* ------------------------------------------------------------------ */

#define EN_PARSER_TASK_STACK    6144
#define EN_PARSER_TASK_PRIO     10
#define EN_RX_TASK_STACK        4096
#define EN_RX_TASK_PRIO         11

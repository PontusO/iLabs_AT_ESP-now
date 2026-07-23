/*
 * en_at_config.h
 *
 * ESP-NOW-specific build-time configuration, layered on top of the shared
 * AT-core transport config (at_core_config.h: UART port, pins, baud, flow
 * control, driver buffers). This is design invariant C4: transport config
 * is shared by every AT personality; the subsystem config here is ESP-NOW's
 * alone.
 *
 * Targets: ESP32-C6 and ESP32-C3 (ESP-IDF v5.x) and
 *          ESP8285/ESP8266 (ESP8266_RTOS_SDK v3.4).
 */

#pragma once

/* Shared transport config: UART port/pins/baud/flow/buffers, plus the
 * EN_TARGET_ESP8266 build switch used across the firmware. */
#include "at_core_config.h"

/* ------------------------------------------------------------------ */
/*  Firmware identity                                                  */
/* ------------------------------------------------------------------ */

#define EN_FW_VERSION           "1.1.0"

/*
 * Product identity reported by the LTE-modem-style identity commands
 * (3GPP TS 27.007): AT+CGMI (manufacturer), AT+CGMM (model) and
 * AT+CGMR (firmware revision, = EN_FW_VERSION).
 */
#define EN_MANUFACTURER         "iLabs Electronics"

/* Model string is per-target so each build reports the chip it runs on. */
#if EN_TARGET_ESP8266
#define EN_MODEL                "ESP8285 ESP-NOW"
#elif CONFIG_IDF_TARGET_ESP32C6
#define EN_MODEL                "ESP32-C6 ESP-NOW"
#elif CONFIG_IDF_TARGET_ESP32C3
#define EN_MODEL                "ESP32-C3 ESP-NOW"
#else
#define EN_MODEL                "ESP32 ESP-NOW"
#endif

/* ------------------------------------------------------------------ */
/*  Buffers & limits                                                   */
/* ------------------------------------------------------------------ */

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

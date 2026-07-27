/*
 * SPDX-FileCopyrightText: 2026 Pontus Oldberg <pontus@ilabs.se>
 * SPDX-License-Identifier: MIT
 *
 * at_parser.h - subsystem-agnostic AT command engine.
 *
 * Owns line assembly, the "AT"/"ATE" primitives, argument splitting and
 * the common parse helpers (hex/MAC/uint), the dispatch loop, and the
 * result -> "OK"/"ERROR"/"+xxERR:<n>" mapping. It knows nothing about
 * any particular subsystem: each one registers its own command table
 * (design invariant C1) and supplies an engine config that names its
 * error-URC prefix and code space (C2/C5), so a single engine drives any
 * AT personality and a merged build simply registers both tables.
 *
 * Grammar (spec section 1):
 *   AT+CMD?          query
 *   AT+CMD=<params>  set
 *   AT+CMD           execute
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    AT_EXEC,    /* AT+CMD          */
    AT_QUERY,   /* AT+CMD?         */
    AT_SET,     /* AT+CMD=<params> */
} at_type_t;

/*
 * Handler return convention:
 *   AT_R_OK    - engine prints "OK"
 *   AT_R_DONE  - handler already printed its own terminal response
 *   1..(err_generic-1) - engine prints "<err_prefix>:<n>" then "ERROR"
 *   anything else (incl. err_generic) - engine prints plain "ERROR"
 */
#define AT_R_OK     0
#define AT_R_DONE   (-1)

typedef int (*at_handler_t)(at_type_t type, char *args);

typedef struct {
    const char  *name;      /* command name without the "AT+" prefix */
    at_handler_t handler;
} at_command_t;

/*
 * Engine configuration. Subsystem-agnostic: the ESP-NOW app supplies
 * "+ENERR"/its code space, a second personality supplies its own prefix,
 * and the engine is unchanged either way.
 */
typedef struct {
    size_t       line_max;        /* AT line buffer size (bytes)              */
    const char  *err_prefix;      /* e.g. "+ENERR" -> "<prefix>:<n>" then ERR */
    int          err_generic;     /* codes >= this map to a plain "ERROR"     */
    int          err_unsupported; /* code emitted for an unknown "AT+" cmd    */
    bool         echo_default;    /* ATE state at boot (ATE0/ATE1 override)   */
    uint32_t     task_stack;      /* parser task stack (bytes)                */
    unsigned     task_prio;       /* parser task priority                     */
} at_engine_cfg_t;

/*
 * C1: register a command table. Call once per subsystem before
 * at_parser_start(). Tables are searched in registration order.
 */
void at_register_commands(const at_command_t *table, size_t count);

/*
 * Spawn the parser task. Call after at_uart_init(), after any platform
 * bring-up, and after all at_register_commands() calls.
 */
void at_parser_start(const at_engine_cfg_t *cfg);

/* ---- terminal responses (for handlers that answer themselves) -------- */

void at_resp_ok(void);      /* "OK"    */
void at_resp_error(void);   /* "ERROR" */

/* ---- shared parse helpers (for subsystem command handlers) ----------- */

/* Decode exactly 2*out_len hex chars into out. False on any non-hex char. */
bool at_hex_decode(const char *hex, uint8_t *out, size_t out_len);

/* MAC = exactly 12 hex chars, no separators. */
bool at_parse_mac(const char *s, uint8_t mac[6]);

/* Parse a base-10 unsigned; false unless the whole string is consumed. */
bool at_parse_uint(const char *s, unsigned *out);

/*
 * Split args on commas, in place. Returns the number of fields (up to
 * max), or -1 if there are more fields than max.
 */
int at_split_args(char *args, char *fields[], int max);

/* Format a MAC as 12 uppercase hex chars (out must hold 13 bytes). */
void at_mac_str(const uint8_t mac[6], char out[13]);

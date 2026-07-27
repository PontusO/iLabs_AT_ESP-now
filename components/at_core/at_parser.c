/*
 * SPDX-FileCopyrightText: 2026 Pontus Oldberg <pontus@ilabs.se>
 * SPDX-License-Identifier: MIT
 *
 * at_parser.c - subsystem-agnostic AT command engine.
 *
 * Line assembly and dispatch. Terminal responses are "OK" or "ERROR";
 * specific faults additionally emit "<err_prefix>:<code>" on the line
 * before "ERROR". URCs may be interleaved at any time by other tasks
 * (at_uart serializes lines). The command tables and the error-code
 * space come from the subsystem via at_register_commands() and the
 * at_engine_cfg_t passed to at_parser_start().
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "at_uart.h"
#include "at_parser.h"

static const char *TAG = "at_parser";

/* Engine configuration and mutable echo state, set at at_parser_start(). */
static at_engine_cfg_t s_cfg;
static bool            s_echo;

/* Registered command tables (design invariant C1). */
#define AT_MAX_TABLES 4
static const at_command_t *s_tables[AT_MAX_TABLES];
static size_t              s_table_counts[AT_MAX_TABLES];
static size_t              s_ntables;

/* ---- response helpers ------------------------------------------------ */

void at_resp_ok(void)
{
    at_uart_write_line("OK");
}

void at_resp_error(void)
{
    at_uart_write_line("ERROR");
}

static void resp_err_code(int code)
{
    at_uart_write_line("%s:%d", s_cfg.err_prefix, code);
    at_resp_error();
}

/* ---- parse helpers ---------------------------------------------------- */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool at_hex_decode(const char *hex, uint8_t *out, size_t out_len)
{
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

bool at_parse_mac(const char *s, uint8_t mac[6])
{
    return strlen(s) == 12 && at_hex_decode(s, mac, 6);
}

bool at_parse_uint(const char *s, unsigned *out)
{
    if (*s == '\0') {
        return false;
    }
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*end != '\0') {
        return false;
    }
    *out = (unsigned)v;
    return true;
}

int at_split_args(char *args, char *fields[], int max)
{
    int n = 0;
    if (!args || *args == '\0') {
        return 0;
    }
    char *p = args;
    for (;;) {
        if (n == max) {
            return -1;
        }
        fields[n++] = p;
        char *comma = strchr(p, ',');
        if (!comma) {
            return n;
        }
        *comma = '\0';
        p = comma + 1;
    }
}

void at_mac_str(const uint8_t mac[6], char out[13])
{
    snprintf(out, 13, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ---- registration & dispatch ----------------------------------------- */

void at_register_commands(const at_command_t *table, size_t count)
{
    configASSERT(s_ntables < AT_MAX_TABLES);
    s_tables[s_ntables]       = table;
    s_table_counts[s_ntables] = count;
    s_ntables++;
}

static void handle_result(int r)
{
    if (r == AT_R_OK) {
        at_resp_ok();
    } else if (r == AT_R_DONE) {
        /* handler already answered */
    } else if (r > 0 && r < s_cfg.err_generic) {
        resp_err_code(r);
    } else {
        at_resp_error();
    }
}

static void process_line(char *line)
{
    /* Bare "AT" attention poke. */
    if (strcasecmp(line, "AT") == 0) {
        at_resp_ok();
        return;
    }
    /* Echo control, standard V.250. */
    if (strcasecmp(line, "ATE0") == 0) {
        s_echo = false;
        at_resp_ok();
        return;
    }
    if (strcasecmp(line, "ATE1") == 0) {
        s_echo = true;
        at_resp_ok();
        return;
    }

    if (strncasecmp(line, "AT+", 3) != 0) {
        at_resp_error();
        return;
    }

    char *cmd = line + 3;
    at_type_t type = AT_EXEC;
    char *args = NULL;

    char *sep = strpbrk(cmd, "=?");
    if (sep && *sep == '?') {
        if (*(sep + 1) != '\0') {
            at_resp_error();
            return;
        }
        type = AT_QUERY;
        *sep = '\0';
    } else if (sep && *sep == '=') {
        type = AT_SET;
        *sep = '\0';
        args = sep + 1;
    }

    for (size_t t = 0; t < s_ntables; t++) {
        const at_command_t *table = s_tables[t];
        for (size_t i = 0; i < s_table_counts[t]; i++) {
            if (strcasecmp(cmd, table[i].name) == 0) {
                handle_result(table[i].handler(type, args));
                return;
            }
        }
    }

    /* Unknown AT+ command: distinct code so hosts can detect version
     * skew (spec section 7, "version skew"). */
    resp_err_code(s_cfg.err_unsupported);
}

/* ---- parser task ----------------------------------------------------- */

static void parser_task(void *arg)
{
    (void)arg;

    char *line = malloc(s_cfg.line_max);
    configASSERT(line);
    size_t pos = 0;
    bool overflow = false;

    uint8_t chunk[64];

    for (;;) {
        /* Block for the first byte, then drain whatever else is already
         * buffered (uart_read_bytes waits for the FULL length otherwise). */
        int n = at_uart_read(chunk, 1, portMAX_DELAY);
        if (n == 1) {
            n += at_uart_read(chunk + 1, sizeof(chunk) - 1, 0);
        }
        for (int i = 0; i < n; i++) {
            char c = (char)chunk[i];

            if (c == '\r' || c == '\n') {
                if (s_echo) {
                    at_uart_write("\r\n", 2);
                }
                if (overflow) {
                    at_resp_error();
                } else if (pos > 0) {
                    line[pos] = '\0';
                    process_line(line);
                }
                pos = 0;
                overflow = false;
                continue;
            }

            /* Minimal line editing for interactive bring-up. */
            if (c == '\b' || c == 0x7F) {
                if (pos > 0) {
                    pos--;
                    if (s_echo) {
                        at_uart_write("\b \b", 3);
                    }
                }
                continue;
            }

            if (pos < s_cfg.line_max - 1) {
                line[pos++] = c;
                if (s_echo) {
                    at_uart_write(&c, 1);
                }
            } else {
                overflow = true;
            }
        }
    }
}

void at_parser_start(const at_engine_cfg_t *cfg)
{
    configASSERT(cfg && cfg->err_prefix && cfg->line_max > 1);
    s_cfg  = *cfg;
    s_echo = cfg->echo_default;

    BaseType_t ok = xTaskCreate(parser_task, "at_parser",
                                cfg->task_stack, NULL,
                                cfg->task_prio, NULL);
    configASSERT(ok == pdPASS);
    ESP_LOGI(TAG, "parser started");
}

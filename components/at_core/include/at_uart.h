/*
 * SPDX-FileCopyrightText: 2026 Pontus Oldberg <pontus@ilabs.se>
 * SPDX-License-Identifier: MIT
 *
 * at_uart.h - UART transport for the AT interpreter.
 *
 * Thin, thread-safe wrapper around the ESP-IDF UART driver. All output
 * (responses and URCs) funnels through at_uart_write_line()/at_uart_write()
 * which serialize on a mutex so a URC can never be interleaved into the
 * middle of a command response line.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"

/* Install the UART driver per at_core_config.h (port, pins, flow control). */
void at_uart_init(void);

/* Raw write, mutex-protected. */
void at_uart_write(const void *data, size_t len);

/* printf-style write of a single line; "\r\n" is appended. Mutex-protected. */
void at_uart_write_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/*
 * Read up to len bytes with a per-call timeout.
 * Returns the number of bytes read (0 on timeout).
 */
int at_uart_read(uint8_t *buf, size_t len, TickType_t ticks_to_wait);

/* Currently configured baud rate of the AT link. */
int at_uart_get_baud(void);

/*
 * Switch the AT link baud rate. Drains the TX FIFO first so any
 * response already queued (e.g. the "OK" acknowledging the change)
 * still leaves at the old rate. Returns 0 on success, -1 on failure.
 */
int at_uart_set_baud(int baud);

/* Currently configured hardware flow control mode (AT_UART_FLOWCTRL_*). */
int at_uart_get_flowctrl(void);

/*
 * Switch the AT link hardware flow control at runtime. `mode` is one of
 * the AT_UART_FLOWCTRL_* values in at_core_config.h. On ESP32-C3/C6 the
 * RTS/CTS GPIOs are routed on demand (they need not have been enabled at
 * build time). Drains the TX FIFO first so a response already queued
 * (e.g. the "OK" acknowledging the change) still leaves before flow
 * control engages. Returns 0 on success, -1 on failure.
 */
int at_uart_set_flowctrl(int mode);

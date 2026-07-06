/*
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

/* Install the UART driver per en_at_config.h (port, pins, flow control). */
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

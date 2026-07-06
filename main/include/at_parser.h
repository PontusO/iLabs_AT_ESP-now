/*
 * at_parser.h - AT command line assembly and dispatch.
 */

#pragma once

/* Spawns the parser task; call after at_uart_init() and en_core_boot(). */
void at_parser_start(void);

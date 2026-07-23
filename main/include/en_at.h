/*
 * en_at.h - ESP-NOW AT command surface for the shared at_core engine.
 */

#pragma once

#include "at_parser.h"

/* Engine config for the ESP-NOW personality (error prefix, code space,
 * line length, parser task tuning). Passed to at_parser_start(). */
extern const at_engine_cfg_t en_at_engine_cfg;

/* Register the AT+EN... command table with the engine. Call before
 * at_parser_start(). */
void en_at_register(void);

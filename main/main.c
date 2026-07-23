/*
 * main.c - ESP-NOW AT interpreter entry point.
 *
 * Boot order: platform prerequisites (NVS/netif/event loop, RX worker),
 * then the AT UART, then the parser. The radio itself stays down until
 * the host issues AT+ENINIT.
 */

#include "esp_log.h"

#include "en_at_config.h"
#include "at_uart.h"
#include "at_parser.h"
#include "en_at.h"
#include "en_core.h"

void app_main(void)
{
    en_core_boot();
    at_uart_init();
    en_at_register();
    at_parser_start(&en_at_engine_cfg);

#if EN_URC_READY_ON_BOOT
    /* Boot marker so the host can synchronize after a slave reset. */
    at_uart_write_line("+ENREADY");
#endif

    ESP_LOGI("main", "ESP-NOW AT interpreter %s ready", EN_FW_VERSION);
}

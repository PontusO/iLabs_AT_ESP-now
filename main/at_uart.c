/*
 * at_uart.c - UART transport for the AT interpreter.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"

#include "en_at_config.h"
#include "at_uart.h"

static const char *TAG = "at_uart";

static SemaphoreHandle_t s_tx_mutex;
static int s_baud = EN_UART_BAUD;

static uart_hw_flowcontrol_t flowctrl_mode(void)
{
    switch (EN_UART_FLOWCTRL) {
    case EN_UART_FLOWCTRL_RTS:     return UART_HW_FLOWCTRL_RTS;
    case EN_UART_FLOWCTRL_CTS:     return UART_HW_FLOWCTRL_CTS;
    case EN_UART_FLOWCTRL_CTS_RTS: return UART_HW_FLOWCTRL_CTS_RTS;
    default:                       return UART_HW_FLOWCTRL_DISABLE;
    }
}

void at_uart_init(void)
{
#if EN_TARGET_ESP8266
    /* ESP8266_RTOS_SDK: pins are fixed by the IO mux and configured by
     * uart_param_config() itself (incl. RTS=GPIO15 / CTS=GPIO13 when
     * flow control is enabled); there is no uart_set_pin() and no
     * source_clk field. */
    uart_config_t cfg = {
        .baud_rate  = EN_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = flowctrl_mode(),
        .rx_flow_ctrl_thresh = EN_UART_RTS_THRESH,
    };

    ESP_ERROR_CHECK(uart_driver_install(EN_UART_PORT,
                                        EN_UART_RX_BUF_SIZE,
                                        EN_UART_TX_BUF_SIZE,
                                        0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(EN_UART_PORT, &cfg));
#if EN_UART_SWAP_IO
    /* Move UART0 to GPIO15(TX)/GPIO13(RX): keeps the ROM's 74880-baud
     * boot output off the AT link. */
    ESP_ERROR_CHECK(uart_enable_swap());
#endif
#else
    const uart_config_t cfg = {
        .baud_rate  = EN_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = flowctrl_mode(),
        .rx_flow_ctrl_thresh = EN_UART_RTS_THRESH,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(EN_UART_PORT,
                                        EN_UART_RX_BUF_SIZE,
                                        EN_UART_TX_BUF_SIZE,
                                        0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(EN_UART_PORT, &cfg));

    int rts = (EN_UART_FLOWCTRL == EN_UART_FLOWCTRL_RTS ||
               EN_UART_FLOWCTRL == EN_UART_FLOWCTRL_CTS_RTS)
                  ? EN_UART_RTS_PIN : UART_PIN_NO_CHANGE;
    int cts = (EN_UART_FLOWCTRL == EN_UART_FLOWCTRL_CTS ||
               EN_UART_FLOWCTRL == EN_UART_FLOWCTRL_CTS_RTS)
                  ? EN_UART_CTS_PIN : UART_PIN_NO_CHANGE;

    ESP_ERROR_CHECK(uart_set_pin(EN_UART_PORT,
                                 EN_UART_TX_PIN, EN_UART_RX_PIN, rts, cts));
#endif

    s_tx_mutex = xSemaphoreCreateMutex();
    configASSERT(s_tx_mutex);

    ESP_LOGI(TAG, "AT UART%d up: %d baud, TX=%d RX=%d flowctrl=%d",
             EN_UART_PORT, EN_UART_BAUD, EN_UART_TX_PIN, EN_UART_RX_PIN,
             EN_UART_FLOWCTRL);
}

void at_uart_write(const void *data, size_t len)
{
    if (len == 0) {
        return;
    }
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uart_write_bytes(EN_UART_PORT, data, len);
    xSemaphoreGive(s_tx_mutex);
}

void at_uart_write_line(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        return;
    }

    char *buf = malloc((size_t)need + 3);
    if (!buf) {
        ESP_LOGE(TAG, "OOM formatting line");
        return;
    }

    va_start(ap, fmt);
    vsnprintf(buf, (size_t)need + 1, fmt, ap);
    va_end(ap);
    buf[need]     = '\r';
    buf[need + 1] = '\n';

    at_uart_write(buf, (size_t)need + 2);
    free(buf);
}

int at_uart_read(uint8_t *buf, size_t len, TickType_t ticks_to_wait)
{
    int n = uart_read_bytes(EN_UART_PORT, buf, len, ticks_to_wait);
    return (n < 0) ? 0 : n;
}

int at_uart_get_baud(void)
{
    return s_baud;
}

int at_uart_set_baud(int baud)
{
    /* Hold the TX mutex so no writer can queue more output between the
     * drain and the rate switch. */
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uart_wait_tx_done(EN_UART_PORT, pdMS_TO_TICKS(1000));

    esp_err_t err = uart_set_baudrate(EN_UART_PORT, (uint32_t)baud);
    if (err == ESP_OK) {
        s_baud = baud;
    }
    xSemaphoreGive(s_tx_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "baud change to %d failed: %s", baud, esp_err_to_name(err));
        return -1;
    }
    ESP_LOGI(TAG, "AT UART baud now %d", baud);
    return 0;
}

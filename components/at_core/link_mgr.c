/*
 * SPDX-FileCopyrightText: 2026 Pontus Oldberg <pontus@ilabs.se>
 * SPDX-License-Identifier: MIT
 *
 * link_mgr.c - the single radio/link owner (design invariant C3).
 *
 * The ESP-NOW bring-up ladder here is a verbatim extraction of what
 * en_init()/en_deinit() used to do inline, including the exact per-stage
 * failure cleanup, so the refactor is behaviour-preserving.
 */

#include "freertos/FreeRTOS.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "link_mgr.h"

static const char *TAG = "link_mgr";

static bool        s_platform_ready;
static link_mode_t s_mode = LINK_MODE_NONE;

void link_mgr_init(void)
{
    if (s_platform_ready) {
        return;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_platform_ready = true;
}

int link_mgr_bring_up(link_mode_t mode, int channel)
{
    switch (mode) {
    case LINK_MODE_ESPNOW: {
        /* Minimal WiFi-STA path: no netif is attached, ESP-NOW needs no IP. */
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&cfg) != ESP_OK) {
            return -1;
        }
        if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
            esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
            esp_wifi_start() != ESP_OK) {
            esp_wifi_deinit();
            return -1;
        }
        esp_wifi_set_ps(WIFI_PS_NONE);      /* ESP-NOW peers can't buffer for us */

        if (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
            esp_wifi_stop();
            esp_wifi_deinit();
            return -1;
        }
        s_mode = mode;
        ESP_LOGI(TAG, "link up: ESP-NOW (WiFi STA) on channel %d", channel);
        return 0;
    }

    case LINK_MODE_MATTER:
        /* Full WiFi + netif + IP path lands in Phase B3 (mirror of the
         * ESP-NOW path above, plus esp_netif_create_default_wifi_sta()). */
        ESP_LOGE(TAG, "LINK_MODE_MATTER not implemented yet");
        return -1;

    default:
        return -1;
    }
}

void link_mgr_tear_down(void)
{
    if (s_mode == LINK_MODE_NONE) {
        return;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    s_mode = LINK_MODE_NONE;
    ESP_LOGI(TAG, "link down");
}

link_mode_t link_mgr_mode(void)
{
    return s_mode;
}

int link_mgr_set_channel(int channel)
{
    return (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) == ESP_OK)
               ? 0 : -1;
}

int link_mgr_get_channel(void)
{
    uint8_t prim = 0;
    wifi_second_chan_t sec;
    if (esp_wifi_get_channel(&prim, &sec) == ESP_OK && prim > 0) {
        return prim;
    }
    return -1;
}

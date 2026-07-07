/*
 * en_core.c - ESP-NOW engine behind the AT interpreter.
 *
 * Over-the-air framing
 * --------------------
 * Every frame sent by this firmware carries a 2-byte prefix so the
 * receive side can tell application data, fragments and the liveness
 * probe apart:
 *
 *   [0] 0xEA magic
 *   [1] frame type
 *
 *   DATA (0x01): [payload ...]                       max 248 bytes payload
 *   FRAG (0x02): [msg_id][idx][total][len_hi][len_lo][chunk ...]
 *   PING (0x03): [token]
 *   PONG (0x04): [token]
 *   DISC (0x05): [token]      discovery probe, always broadcast
 *   DRSP (0x06): [token]      discovery response, unicast to the prober
 *
 * Frames that do not start with the magic (e.g. from a third-party
 * ESP-NOW node) fall through as-is and are reported verbatim via
 * +ENRECV, so plain interop still works in the receive direction.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "en_at_config.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_idf_version.h"
#include "nvs_flash.h"

#if EN_TARGET_ESP8266
#include "esp_system.h"     /* esp_read_mac + esp_random on the 8266 SDK */
#else
#include "esp_mac.h"
#include "esp_random.h"
#endif

#include "at_uart.h"
#include "en_core.h"

static const char *TAG = "en_core";

/* ---- OTA frame format -------------------------------------------- */

#define EN_MAGIC        0xEA
#define EN_T_DATA       0x01
#define EN_T_FRAG       0x02
#define EN_T_PING       0x03
#define EN_T_PONG       0x04
#define EN_T_DISC       0x05
#define EN_T_DISCRESP   0x06

#define EN_HDR_LEN      2
#define EN_FRAG_HDR_LEN 7                                   /* magic..len_lo */
#define EN_DATA_MAX     (ESP_NOW_MAX_DATA_LEN - EN_HDR_LEN)      /* 248 */
#define EN_FRAG_CHUNK   (ESP_NOW_MAX_DATA_LEN - EN_FRAG_HDR_LEN) /* 243 */
#define EN_FRAG_MAX_N   32          /* fragments per message (bitmask) */

_Static_assert(EN_FRAG_MAX_TOTAL <= EN_FRAG_MAX_N * EN_FRAG_CHUNK,
               "EN_FRAG_MAX_TOTAL exceeds what 32 fragments can carry");

static const uint8_t BCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* ---- state -------------------------------------------------------- */

static bool                s_booted;
static volatile bool       s_init;
static volatile en_state_t s_state = EN_STATE_UNINIT;
static int                 s_channel = 1;
static int                 s_rate = -1;       /* -1 = driver default */
static bool                s_pmk_set;
static uint8_t             s_pmk[16];

static en_stats_t          s_stats;

/* send-callback rendezvous (one AT command in flight at a time) */
static SemaphoreHandle_t   s_send_sem;
static uint8_t             s_wait_mac[6];
static volatile bool       s_waiting;
static volatile bool       s_send_ok;

/* ping/pong rendezvous */
static SemaphoreHandle_t   s_pong_sem;
static volatile uint8_t    s_ping_token;
static uint8_t             s_ping_mac[6];

/* discovery scan state (written by RX task, read by en_discover) */
typedef struct {
    uint8_t mac[6];
    int     rssi;
} disc_entry_t;
static disc_entry_t        s_disc[EN_DISCOVER_MAX];
static volatile int        s_disc_count;
static volatile bool       s_disc_active;
static volatile uint8_t    s_disc_token;
static volatile bool       s_discoverable = (EN_DISCOVERY_RESPOND != 0);

/* last-frame RSSI, global and per peer */
static bool                s_rssi_valid;
static int                 s_rssi_last;

typedef struct {
    bool    valid;
    uint8_t mac[6];
    int     rssi;
} peer_rssi_t;
static peer_rssi_t s_peer_rssi[ESP_NOW_MAX_TOTAL_PEER_NUM];

/* WiFi-task -> worker-task receive hand-off */
typedef struct {
    uint8_t  mac[6];
    int      rssi;
    uint16_t len;
    uint8_t *data;
} rx_item_t;
static QueueHandle_t s_rx_queue;

/* fragment reassembly */
typedef struct {
    bool     in_use;
    uint8_t  mac[6];
    uint8_t  msg_id;
    uint8_t  total_frags;
    uint32_t recv_mask;
    uint16_t total_len;
    uint8_t *buf;
    int64_t  last_us;
    int      rssi;
} frag_slot_t;
static frag_slot_t s_frag[EN_FRAG_RX_SLOTS];
static uint8_t     s_frag_msg_id;

/* ---- small helpers ------------------------------------------------ */

static void mac_to_str(const uint8_t mac[6], char out[13])
{
    snprintf(out, 13, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void note_rssi(const uint8_t mac[6], int rssi)
{
#if EN_TARGET_ESP8266
    /* The 8266 SDK's ESP-NOW receive callback carries no RSSI, so the
     * tracker stays invalid and AT+ENRSSI reports ERROR. */
    (void)mac;
    (void)rssi;
    return;
#else
    s_rssi_last  = rssi;
    s_rssi_valid = true;

    int free_slot = -1;
    for (int i = 0; i < (int)(sizeof(s_peer_rssi) / sizeof(s_peer_rssi[0])); i++) {
        if (s_peer_rssi[i].valid &&
            memcmp(s_peer_rssi[i].mac, mac, 6) == 0) {
            s_peer_rssi[i].rssi = rssi;
            return;
        }
        if (!s_peer_rssi[i].valid && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot >= 0) {
        s_peer_rssi[free_slot].valid = true;
        memcpy(s_peer_rssi[free_slot].mac, mac, 6);
        s_peer_rssi[free_slot].rssi = rssi;
    }
#endif
}

/* Emit +ENRECV:<src_mac>,<len>,<rssi>,<payload_hex> */
static void urc_recv(const uint8_t mac[6], int rssi,
                     const uint8_t *data, size_t len)
{
    char macs[13];
    mac_to_str(mac, macs);

    size_t need = 32 + 12 + 2 * len;
    char *line = malloc(need);
    if (!line) {
        ESP_LOGE(TAG, "OOM for +ENRECV (%u bytes)", (unsigned)len);
        s_stats.rx_drop++;
        return;
    }
    static const char hexd[] = "0123456789ABCDEF";
    int off = snprintf(line, need, "+ENRECV:%s,%u,%d,", macs, (unsigned)len, rssi);
    for (size_t i = 0; i < len; i++) {
        line[off++] = hexd[data[i] >> 4];
        line[off++] = hexd[data[i] & 0x0F];
    }
    line[off] = '\0';
    at_uart_write_line("%s", line);
    free(line);
}

/* ---- ESP-NOW callbacks (run in the WiFi task - keep them short) --- */

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
static void send_cb(const wifi_tx_info_t *tx_info, esp_now_send_status_t status)
{
    const uint8_t *mac_addr = tx_info->des_addr;
#else
static void send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
#endif
    if (s_waiting && memcmp(mac_addr, s_wait_mac, 6) == 0) {
        s_send_ok  = (status == ESP_NOW_SEND_SUCCESS);
        s_waiting  = false;
        xSemaphoreGive(s_send_sem);
    }
}

static void enqueue_rx(const uint8_t *src_mac, int rssi,
                       const uint8_t *data, int len)
{
    if (len <= 0 || !s_rx_queue) {
        return;
    }

    rx_item_t item;
    memcpy(item.mac, src_mac, 6);
    item.rssi = rssi;
    item.len  = (uint16_t)len;
    item.data = malloc(len);
    if (!item.data) {
        s_stats.rx_drop++;
        return;
    }
    memcpy(item.data, data, len);

    if (xQueueSend(s_rx_queue, &item, 0) != pdTRUE) {
        free(item.data);
        s_stats.rx_drop++;
    }
}

#if EN_TARGET_ESP8266
/* 8266 SDK callback: source MAC only - no des_addr, no RSSI. */
static void recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len)
{
    enqueue_rx(mac_addr, 0, data, len);
}
#else
static void recv_cb(const esp_now_recv_info_t *info,
                    const uint8_t *data, int len)
{
    enqueue_rx(info->src_addr, info->rx_ctrl ? info->rx_ctrl->rssi : 0,
               data, len);
}
#endif

/* ---- fragment reassembly (RX worker task context) ----------------- */

static void frag_slot_free(frag_slot_t *s)
{
    free(s->buf);
    memset(s, 0, sizeof(*s));
}

static void frag_expire_stale(int64_t now_us)
{
    for (int i = 0; i < EN_FRAG_RX_SLOTS; i++) {
        if (s_frag[i].in_use &&
            now_us - s_frag[i].last_us > (int64_t)EN_FRAG_TIMEOUT_MS * 1000) {
            char macs[13];
            mac_to_str(s_frag[i].mac, macs);
            ESP_LOGW(TAG, "reassembly timeout from %s (msg %u)",
                     macs, s_frag[i].msg_id);
            s_stats.rx_drop++;
            frag_slot_free(&s_frag[i]);
        }
    }
}

static void handle_frag_frame(const rx_item_t *it)
{
    const uint8_t *p = it->data;
    if (it->len <= EN_FRAG_HDR_LEN) {
        s_stats.rx_drop++;
        return;
    }

    uint8_t  msg_id    = p[2];
    uint8_t  idx       = p[3];               /* 0-based on the wire */
    uint8_t  total     = p[4];
    uint16_t total_len = ((uint16_t)p[5] << 8) | p[6];
    size_t   chunk_len = it->len - EN_FRAG_HDR_LEN;

    if (total == 0 || total > EN_FRAG_MAX_N || idx >= total ||
        total_len == 0 || total_len > EN_FRAG_MAX_TOTAL ||
        total_len > (size_t)total * EN_FRAG_CHUNK) {
        s_stats.rx_drop++;
        return;
    }

    size_t offset   = (size_t)idx * EN_FRAG_CHUNK;
    size_t expected = (idx == total - 1) ? (total_len - offset) : EN_FRAG_CHUNK;
    if (offset >= total_len || chunk_len != expected) {
        s_stats.rx_drop++;
        return;
    }

    int64_t now = esp_timer_get_time();

    /* find or allocate a slot for (src mac, msg_id) */
    frag_slot_t *slot = NULL;
    for (int i = 0; i < EN_FRAG_RX_SLOTS; i++) {
        if (s_frag[i].in_use && s_frag[i].msg_id == msg_id &&
            memcmp(s_frag[i].mac, it->mac, 6) == 0) {
            slot = &s_frag[i];
            break;
        }
    }
    if (slot &&
        (slot->total_frags != total || slot->total_len != total_len)) {
        /* header changed mid-message: restart */
        frag_slot_free(slot);
        slot = NULL;
        s_stats.rx_drop++;
    }
    if (!slot) {
        for (int i = 0; i < EN_FRAG_RX_SLOTS; i++) {
            if (!s_frag[i].in_use) {
                slot = &s_frag[i];
                break;
            }
        }
        if (!slot) {
            s_stats.rx_drop++;       /* all slots busy */
            return;
        }
        memset(slot, 0, sizeof(*slot));
        slot->buf = malloc(total_len);
        if (!slot->buf) {
            s_stats.rx_drop++;
            return;
        }
        slot->in_use      = true;
        memcpy(slot->mac, it->mac, 6);
        slot->msg_id      = msg_id;
        slot->total_frags = total;
        slot->total_len   = total_len;
    }

    memcpy(slot->buf + offset, p + EN_FRAG_HDR_LEN, chunk_len);
    slot->recv_mask |= (1u << idx);
    slot->last_us    = now;
    slot->rssi       = it->rssi;
    s_stats.rx_ok++;

#if EN_URC_FRAG_PROGRESS
    {
        char macs[13];
        mac_to_str(it->mac, macs);
        at_uart_write_line("+ENFRAGRECV:%s,%u,%u", macs, idx + 1, total);
    }
#endif

    uint32_t all = (total == 32) ? 0xFFFFFFFFu : ((1u << total) - 1u);
    if (slot->recv_mask == all) {
        note_rssi(it->mac, slot->rssi);
        urc_recv(it->mac, slot->rssi, slot->buf, slot->total_len);
        frag_slot_free(slot);
    }
}

/* ---- discovery (RX worker task context) ---------------------------- */

/*
 * Answer a discovery probe. The prober is usually not in our peer
 * table, so it is added transiently for the unicast reply and removed
 * again afterwards - AT+ENLISTPEER stays unaffected on the responder.
 */
static void discovery_respond(const uint8_t mac[6], uint8_t token)
{
    bool was_peer = esp_now_is_peer_exist(mac);
    if (!was_peer) {
        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, mac, 6);
        peer.channel = 0;               /* current channel */
        peer.ifidx   = WIFI_IF_STA;
        peer.encrypt = false;
        if (esp_now_add_peer(&peer) != ESP_OK) {
            return;                     /* peer table full - stay silent */
        }
    }

#if EN_DISCOVER_JITTER_MS > 0
    /* Spread fleet responses out so they don't all collide on air. */
    vTaskDelay(pdMS_TO_TICKS(esp_random() % (EN_DISCOVER_JITTER_MS + 1)));
#endif

    uint8_t resp[3] = {EN_MAGIC, EN_T_DISCRESP, token};
    esp_now_send(mac, resp, sizeof(resp));

    if (!was_peer) {
        /* Give the frame time to leave before dropping the peer entry. */
        vTaskDelay(pdMS_TO_TICKS(20));
        esp_now_del_peer(mac);
    }
}

static void discovery_collect(const uint8_t mac[6], int rssi)
{
    int n = s_disc_count;
    for (int i = 0; i < n; i++) {
        if (memcmp(s_disc[i].mac, mac, 6) == 0) {
            return;                     /* duplicate response */
        }
    }
    if (n < EN_DISCOVER_MAX) {
        memcpy(s_disc[n].mac, mac, 6);
        s_disc[n].rssi = rssi;
        s_disc_count = n + 1;
    }
}

/* ---- RX worker task ------------------------------------------------ */

static void rx_task(void *arg)
{
    (void)arg;
    rx_item_t it;

    for (;;) {
        if (xQueueReceive(s_rx_queue, &it, pdMS_TO_TICKS(500)) != pdTRUE) {
            frag_expire_stale(esp_timer_get_time());
            continue;
        }

        const uint8_t *p = it.data;

        if (it.len >= EN_HDR_LEN && p[0] == EN_MAGIC) {
            switch (p[1]) {
            case EN_T_DATA:
                note_rssi(it.mac, it.rssi);
                s_stats.rx_ok++;
                urc_recv(it.mac, it.rssi, p + EN_HDR_LEN, it.len - EN_HDR_LEN);
                break;

            case EN_T_FRAG:
                handle_frag_frame(&it);
                break;

            case EN_T_PING:
                if (it.len >= EN_HDR_LEN + 1 && s_init &&
                    esp_now_is_peer_exist(it.mac)) {
                    /* reply in kind; not surfaced to the host at all */
                    uint8_t pong[3] = {EN_MAGIC, EN_T_PONG, p[2]};
                    esp_now_send(it.mac, pong, sizeof(pong));
                }
                note_rssi(it.mac, it.rssi);
                break;

            case EN_T_PONG:
                if (it.len >= EN_HDR_LEN + 1 && p[2] == s_ping_token &&
                    memcmp(it.mac, s_ping_mac, 6) == 0) {
                    note_rssi(it.mac, it.rssi);
                    xSemaphoreGive(s_pong_sem);
                }
                break;

            case EN_T_DISC:
                note_rssi(it.mac, it.rssi);
                if (it.len >= EN_HDR_LEN + 1 && s_init && s_discoverable) {
                    discovery_respond(it.mac, p[2]);
                }
                break;

            case EN_T_DISCRESP:
                note_rssi(it.mac, it.rssi);
                if (it.len >= EN_HDR_LEN + 1 && s_disc_active &&
                    p[2] == s_disc_token) {
                    discovery_collect(it.mac, it.rssi);
                }
                break;

            default:
                s_stats.rx_drop++;
                break;
            }
        } else {
            /* No recognizable header: raw frame from a foreign ESP-NOW
             * node - hand the whole payload to the host untouched. */
            note_rssi(it.mac, it.rssi);
            s_stats.rx_ok++;
            urc_recv(it.mac, it.rssi, p, it.len);
        }

        free(it.data);
        frag_expire_stale(esp_timer_get_time());
    }
}

/* ---- PHY rate ------------------------------------------------------ */

#if !EN_TARGET_ESP8266 && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
static wifi_phy_mode_t phymode_for_rate(int rate)
{
    if (rate <= 0x07) {
        return WIFI_PHY_MODE_11B;               /* 1/2/5.5/11 Mbps      */
    }
    if (rate <= 0x0F) {
        return WIFI_PHY_MODE_11G;               /* 6..54 Mbps OFDM      */
    }
    if (rate >= WIFI_PHY_RATE_MCS0_LGI && rate <= WIFI_PHY_RATE_MCS7_SGI) {
        return WIFI_PHY_MODE_HT20;              /* 802.11n MCS0..7      */
    }
    if (rate == WIFI_PHY_RATE_LORA_250K || rate == WIFI_PHY_RATE_LORA_500K) {
        return WIFI_PHY_MODE_LR;                /* Espressif long range */
    }
    return WIFI_PHY_MODE_11G;
}

static esp_err_t apply_rate_to_peer(const uint8_t mac[6])
{
    if (s_rate < 0) {
        return ESP_OK;
    }
    esp_now_rate_config_t rc = {
        .phymode = phymode_for_rate(s_rate),
        .rate    = (wifi_phy_rate_t)s_rate,
    };
    return esp_now_set_peer_rate_config(mac, &rc);
}
#endif

/* ---- boot / lifecycle ---------------------------------------------- */

void en_core_boot(void)
{
    if (s_booted) {
        return;
    }

    esp_err_t err = nvs_flash_init();
#if EN_TARGET_ESP8266
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
#else
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
#endif
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_send_sem = xSemaphoreCreateBinary();
    s_pong_sem = xSemaphoreCreateBinary();
    s_rx_queue = xQueueCreate(EN_RX_QUEUE_LEN, sizeof(rx_item_t));
    configASSERT(s_send_sem && s_pong_sem && s_rx_queue);

    BaseType_t ok = xTaskCreate(rx_task, "en_rx", EN_RX_TASK_STACK,
                                NULL, EN_RX_TASK_PRIO, NULL);
    configASSERT(ok == pdPASS);

    s_booted = true;
}

int en_init(int channel)
{
    if (channel < 1 || channel > 14) {
        return EN_ERR_GENERIC;
    }
    if (s_init) {
        /* Idempotent re-init: just retune the channel. */
        return en_set_channel(channel);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        goto fail;
    }
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
        esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        goto fail_deinit_wifi;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);      /* ESP-NOW peers can't buffer for us */

    if (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        goto fail_stop_wifi;
    }
    if (esp_now_init() != ESP_OK ||
        esp_now_register_send_cb(send_cb) != ESP_OK ||
        esp_now_register_recv_cb(recv_cb) != ESP_OK) {
        esp_now_deinit();
        goto fail_stop_wifi;
    }

    s_channel = channel;
    s_init    = true;
    s_state   = EN_STATE_IDLE;
    memset(&s_stats, 0, sizeof(s_stats));
    ESP_LOGI(TAG, "ESP-NOW up on channel %d", channel);
    return EN_OK;

fail_stop_wifi:
    esp_wifi_stop();
fail_deinit_wifi:
    esp_wifi_deinit();
fail:
    s_state = EN_STATE_ERROR;
    return EN_ERR_INIT;
}

int en_deinit(void)
{
    if (!s_init) {
        s_state = EN_STATE_UNINIT;
        return EN_OK;
    }

    s_init = false;
    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();

    /* Drop anything still queued for the RX worker. In-flight
     * reassembly slots belong to the RX task and are reclaimed there by
     * frag_expire_stale() - freeing them here would race it. */
    rx_item_t it;
    while (xQueueReceive(s_rx_queue, &it, 0) == pdTRUE) {
        free(it.data);
    }

    s_pmk_set = false;
    s_rate    = -1;
    s_state   = EN_STATE_UNINIT;
    ESP_LOGI(TAG, "ESP-NOW down");
    return EN_OK;
}

bool en_is_init(void)
{
    return s_init;
}

en_state_t en_get_state(void)
{
    return s_state;
}

/* ---- radio config --------------------------------------------------- */

int en_set_channel(int channel)
{
    if (channel < 1 || channel > 14) {
        return EN_ERR_GENERIC;
    }
    if (!s_init) {
        return EN_ERR_NOT_INIT;
    }
    if (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        return EN_ERR_GENERIC;
    }
    s_channel = channel;
    return EN_OK;
}

int en_get_channel(void)
{
    if (s_init) {
        uint8_t prim = 0;
        wifi_second_chan_t sec;
        if (esp_wifi_get_channel(&prim, &sec) == ESP_OK && prim > 0) {
            s_channel = prim;
        }
    }
    return s_channel;
}

int en_set_rate(int rate_idx)
{
#if EN_TARGET_ESP8266
    /* The 8266 SDK exposes no ESP-NOW rate API at all. */
    (void)rate_idx;
    return EN_ERR_UNSUPPORTED;
#else
    if (!s_init) {
        return EN_ERR_NOT_INIT;
    }
    if (rate_idx < 0 || rate_idx > 0x2A) {
        return EN_ERR_GENERIC;
    }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
    s_rate = rate_idx;

    if (phymode_for_rate(rate_idx) == WIFI_PHY_MODE_LR) {
        /* LR rates require the LR protocol bit on the interface. */
        esp_wifi_set_protocol(WIFI_IF_STA,
                              WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                              WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
    }

    /* Apply to every peer already in the table; new peers pick the
     * rate up in en_add_peer(). */
    esp_now_peer_info_t peer;
    bool from_head = true;
    while (esp_now_fetch_peer(from_head, &peer) == ESP_OK) {
        from_head = false;
        if (apply_rate_to_peer(peer.peer_addr) != ESP_OK) {
            s_rate = -1;
            return EN_ERR_GENERIC;
        }
    }
    return EN_OK;
#else
    if (esp_wifi_config_espnow_rate(WIFI_IF_STA,
                                    (wifi_phy_rate_t)rate_idx) != ESP_OK) {
        return EN_ERR_GENERIC;
    }
    s_rate = rate_idx;
    return EN_OK;
#endif
#endif /* EN_TARGET_ESP8266 */
}

void en_get_mac(uint8_t mac[6])
{
    /* esp_read_mac works before WiFi init, so AT+ENMAC? is usable at
     * first boot for provisioning, ahead of AT+ENINIT. */
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
}

uint32_t en_espnow_version(void)
{
    uint32_t v = 0;
    if (s_init) {
        esp_now_get_version(&v);
    }
    return v;
}

/* ---- peer management ------------------------------------------------- */

int en_add_peer(const uint8_t mac[6], int channel, bool encrypt,
                const uint8_t *lmk)
{
    if (!s_init) {
        return EN_ERR_NOT_INIT;
    }
    if (channel < 0 || channel > 14) {
        return EN_ERR_GENERIC;
    }

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = (uint8_t)channel;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = encrypt;

    if (encrypt) {
        if (lmk) {
            memcpy(peer.lmk, lmk, 16);
        } else if (s_pmk_set) {
            /* Convention (both ends run this firmware): when no LMK is
             * given, the PMK doubles as the LMK so the spec's
             * PMK-then-ADDPEER flow yields interoperable encryption. */
            memcpy(peer.lmk, s_pmk, 16);
        } else {
            return EN_ERR_BAD_KEY;
        }
    }

    esp_err_t err = esp_now_add_peer(&peer);
    if (err == ESP_ERR_ESPNOW_EXIST) {
        err = esp_now_mod_peer(&peer);
    }
    if (err == ESP_ERR_ESPNOW_FULL) {
        return EN_ERR_PEER_FULL;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "add_peer: %s", esp_err_to_name(err));
        return EN_ERR_GENERIC;
    }

#if !EN_TARGET_ESP8266 && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
    apply_rate_to_peer(mac);
#endif
    return EN_OK;
}

int en_del_peer(const uint8_t mac[6])
{
    if (!s_init) {
        return EN_ERR_NOT_INIT;
    }
    return (esp_now_del_peer(mac) == ESP_OK) ? EN_OK : EN_ERR_GENERIC;
}

int en_list_peers(void (*cb)(const uint8_t mac[6], int channel, bool encrypt))
{
    if (!s_init) {
        return EN_ERR_NOT_INIT;
    }
    esp_now_peer_info_t peer;
    bool from_head = true;
    while (esp_now_fetch_peer(from_head, &peer) == ESP_OK) {
        from_head = false;
        cb(peer.peer_addr, peer.channel, peer.encrypt);
    }
    return EN_OK;
}

int en_set_pmk(const uint8_t pmk[16])
{
    if (!s_init) {
        return EN_ERR_NOT_INIT;
    }
    if (esp_now_set_pmk(pmk) != ESP_OK) {
        return EN_ERR_BAD_KEY;
    }
    memcpy(s_pmk, pmk, 16);
    s_pmk_set = true;
    return EN_OK;
}

int en_set_lmk(const uint8_t mac[6], const uint8_t lmk[16])
{
    if (!s_init) {
        return EN_ERR_NOT_INIT;
    }
    esp_now_peer_info_t peer;
    if (esp_now_get_peer(mac, &peer) != ESP_OK) {
        return EN_ERR_GENERIC;
    }
    memcpy(peer.lmk, lmk, 16);
    peer.encrypt = true;
    if (esp_now_mod_peer(&peer) != ESP_OK) {
        return EN_ERR_BAD_KEY;
    }
    return EN_OK;
}

/* ---- data path -------------------------------------------------------- */

size_t en_max_payload(void)
{
    return EN_DATA_MAX;
}

size_t en_max_frag_payload(void)
{
    return EN_FRAG_MAX_TOTAL;
}

/* Queue one raw ESP-NOW frame and arm the send-callback rendezvous. */
static int ll_send_start(const uint8_t mac[6], const uint8_t *frame, size_t len)
{
    memcpy(s_wait_mac, mac, 6);
    xSemaphoreTake(s_send_sem, 0);      /* clear any stale give */
    s_waiting = true;

    esp_err_t err = esp_now_send(mac, frame, len);
    if (err != ESP_OK) {
        s_waiting = false;
        ESP_LOGW(TAG, "esp_now_send: %s", esp_err_to_name(err));
        return (err == ESP_ERR_ESPNOW_NOT_FOUND) ? EN_ERR_GENERIC
                                                 : EN_ERR_UNREACHABLE;
    }
    return EN_OK;
}

/*
 * Wait for the send callback of the frame queued by ll_send_start().
 * count_stats is false for control traffic (ping/pong) so that
 * AT+ENPEERCHECK does not pollute the application counters.
 */
static int ll_send_finish(bool count_stats)
{
    bool ok = false;
    if (xSemaphoreTake(s_send_sem, pdMS_TO_TICKS(EN_SEND_CB_TIMEOUT_MS)) == pdTRUE) {
        ok = s_send_ok;
    } else {
        s_waiting = false;
    }
    if (count_stats) {
        if (ok) {
            s_stats.tx_ok++;
        } else {
            s_stats.tx_fail++;
        }
    }
    return ok ? EN_OK : EN_ERR_UNREACHABLE;
}

static int data_send_start(const uint8_t mac[6],
                           const uint8_t *data, size_t len)
{
    if (!s_init) {
        return EN_ERR_NOT_INIT;
    }
    if (len > EN_DATA_MAX) {
        return EN_ERR_TOO_LARGE;
    }

    uint8_t frame[ESP_NOW_MAX_DATA_LEN];
    frame[0] = EN_MAGIC;
    frame[1] = EN_T_DATA;
    memcpy(frame + EN_HDR_LEN, data, len);

    s_state = EN_STATE_SENDING;
    int r = ll_send_start(mac, frame, len + EN_HDR_LEN);
    if (r != EN_OK) {
        s_state = EN_STATE_IDLE;
        s_stats.tx_fail++;
    }
    return r;
}

int en_send_start(const uint8_t mac[6], const uint8_t *data, size_t len)
{
    return data_send_start(mac, data, len);
}

/* ESP-NOW requires even the broadcast address to be in the peer table;
 * register it on first use. */
static int ensure_bcast_peer(void)
{
    if (esp_now_is_peer_exist(BCAST_MAC)) {
        return EN_OK;
    }
    return en_add_peer(BCAST_MAC, 0, false, NULL);
}

int en_bcast_start(const uint8_t *data, size_t len)
{
    if (!s_init) {
        return EN_ERR_NOT_INIT;
    }
    int r = ensure_bcast_peer();
    if (r != EN_OK) {
        return r;
    }
    return data_send_start(BCAST_MAC, data, len);
}

int en_send_finish(void)
{
    int r = ll_send_finish(true);
    s_state = EN_STATE_IDLE;
    return r;
}

int en_frag_send(const uint8_t mac[6], const uint8_t *data, size_t total_len)
{
    if (!s_init) {
        return EN_ERR_NOT_INIT;
    }
    if (total_len == 0 || total_len > EN_FRAG_MAX_TOTAL) {
        return EN_ERR_TOO_LARGE;
    }

    uint8_t total = (uint8_t)((total_len + EN_FRAG_CHUNK - 1) / EN_FRAG_CHUNK);
    uint8_t msg_id = ++s_frag_msg_id;
    char macs[13];
    mac_to_str(mac, macs);

    s_state = EN_STATE_SENDING;

    uint8_t frame[ESP_NOW_MAX_DATA_LEN];
    for (uint8_t idx = 0; idx < total; idx++) {
        size_t offset = (size_t)idx * EN_FRAG_CHUNK;
        size_t chunk  = (idx == total - 1) ? (total_len - offset)
                                           : (size_t)EN_FRAG_CHUNK;
        frame[0] = EN_MAGIC;
        frame[1] = EN_T_FRAG;
        frame[2] = msg_id;
        frame[3] = idx;
        frame[4] = total;
        frame[5] = (uint8_t)(total_len >> 8);
        frame[6] = (uint8_t)(total_len & 0xFF);
        memcpy(frame + EN_FRAG_HDR_LEN, data + offset, chunk);

        int r = ll_send_start(mac, frame, chunk + EN_FRAG_HDR_LEN);
        if (r == EN_OK) {
            r = ll_send_finish(true);
        } else {
            s_stats.tx_fail++;
        }
        if (r != EN_OK) {
            /* fail-fast: no partial-delivery URCs, caller reports FAIL */
            s_state = EN_STATE_IDLE;
            return EN_ERR_UNREACHABLE;
        }

#if EN_URC_FRAG_PROGRESS
        at_uart_write_line("+ENFRAGRECV:%s,%u,%u", macs, idx + 1, total);
#endif
    }

    s_state = EN_STATE_IDLE;
    return EN_OK;
}

int en_discover(int timeout_ms, void (*cb)(const uint8_t mac[6], int rssi))
{
    if (!s_init) {
        return EN_ERR_NOT_INIT;
    }
    int r = ensure_bcast_peer();
    if (r != EN_OK) {
        return r;
    }

    s_disc_count  = 0;
    s_disc_token++;
    s_disc_active = true;

    uint8_t probe[3] = {EN_MAGIC, EN_T_DISC, s_disc_token};
    r = ll_send_start(BCAST_MAC, probe, sizeof(probe));
    if (r == EN_OK) {
        r = ll_send_finish(false);      /* control frame - not counted */
    }
    if (r != EN_OK) {
        s_disc_active = false;
        return EN_ERR_UNREACHABLE;
    }

    /* Collection window: the RX task fills s_disc as responses land. */
    vTaskDelay(pdMS_TO_TICKS(timeout_ms));
    s_disc_active = false;

    for (int i = 0; i < s_disc_count; i++) {
        cb(s_disc[i].mac, s_disc[i].rssi);
    }
    return EN_OK;
}

void en_set_discoverable(bool on)
{
    s_discoverable = on;
}

bool en_get_discoverable(void)
{
    return s_discoverable;
}

/* ---- diagnostics ------------------------------------------------------- */

int en_peer_check(const uint8_t mac[6], int *rtt_ms)
{
    if (!s_init) {
        return EN_ERR_NOT_INIT;
    }

    memcpy(s_ping_mac, mac, 6);
    s_ping_token++;
    xSemaphoreTake(s_pong_sem, 0);      /* clear any stale give */

    uint8_t ping[3] = {EN_MAGIC, EN_T_PING, s_ping_token};
    int64_t t0 = esp_timer_get_time();

    int r = ll_send_start(mac, ping, sizeof(ping));
    if (r != EN_OK) {
        return EN_ERR_UNREACHABLE;
    }
    if (ll_send_finish(false) != EN_OK) {
        return EN_ERR_UNREACHABLE;
    }
    if (xSemaphoreTake(s_pong_sem, pdMS_TO_TICKS(EN_PING_TIMEOUT_MS)) != pdTRUE) {
        return EN_ERR_UNREACHABLE;
    }

    *rtt_ms = (int)((esp_timer_get_time() - t0) / 1000);
    return EN_OK;
}

void en_get_stats(en_stats_t *out)
{
    *out = s_stats;
}

bool en_get_rssi_last(int *rssi)
{
    if (!s_rssi_valid) {
        return false;
    }
    *rssi = s_rssi_last;
    return true;
}

bool en_get_rssi_peer(const uint8_t mac[6], int *rssi)
{
    for (int i = 0; i < (int)(sizeof(s_peer_rssi) / sizeof(s_peer_rssi[0])); i++) {
        if (s_peer_rssi[i].valid &&
            memcmp(s_peer_rssi[i].mac, mac, 6) == 0) {
            *rssi = s_peer_rssi[i].rssi;
            return true;
        }
    }
    return false;
}

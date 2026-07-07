/*
 * en_core.h - ESP-NOW engine behind the AT interpreter.
 *
 * Owns WiFi/ESP-NOW bring-up, the peer table, keys, the data path
 * (single-frame, raw and fragmented sends, receive dispatch), the
 * ping/pong liveness probe and the diagnostic counters.
 *
 * All functions returning int use the +ENERR error code space:
 *   EN_OK           success
 *   EN_ERR_*        specific fault (positive, maps 1:1 to +ENERR:<n>)
 *   EN_ERR_GENERIC  plain ERROR without a +ENERR code
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* +ENERR:<n> codes (see spec section 6). */
#define EN_OK               0
#define EN_ERR_INIT         1   /* WiFi/stack init failure               */
#define EN_ERR_PEER_FULL    2   /* Peer table full                       */
#define EN_ERR_UNREACHABLE  3   /* Peer unreachable / no ACK             */
#define EN_ERR_TOO_LARGE    4   /* Payload too large for mode            */
#define EN_ERR_NOT_INIT     5   /* Command issued before AT+ENINIT       */
#define EN_ERR_BAD_KEY      6   /* Encryption key invalid/missing        */
#define EN_ERR_FRAG_TIMEOUT 7   /* Fragment reassembly timeout           */
#define EN_ERR_UNSUPPORTED  8   /* Unknown/unsupported command           */
#define EN_ERR_GENERIC      100 /* plain ERROR, no +ENERR line           */

/* AT+ENSTATE? values. */
typedef enum {
    EN_STATE_UNINIT  = 0,
    EN_STATE_IDLE    = 1,
    EN_STATE_SENDING = 2,
    EN_STATE_ERROR   = 3,
} en_state_t;

typedef struct {
    uint32_t tx_ok;
    uint32_t tx_fail;
    uint32_t rx_ok;
    uint32_t rx_drop;
} en_stats_t;

/* Called once from app_main before any AT traffic (NVS, netif, events). */
void en_core_boot(void);

/* Lifecycle */
int  en_init(int channel);
int  en_deinit(void);
bool en_is_init(void);
en_state_t en_get_state(void);

/* Radio configuration */
int  en_set_channel(int channel);
int  en_get_channel(void);
int  en_set_rate(int rate_idx);
void en_get_mac(uint8_t mac[6]);
uint32_t en_espnow_version(void);   /* 0 when not initialized */

/* Peer management */
int  en_add_peer(const uint8_t mac[6], int channel, bool encrypt,
                 const uint8_t *lmk /* 16 bytes or NULL */);
int  en_del_peer(const uint8_t mac[6]);
/* Calls cb once per registered peer, returns EN_OK. */
int  en_list_peers(void (*cb)(const uint8_t mac[6], int channel, bool encrypt));
int  en_set_pmk(const uint8_t pmk[16]);
int  en_set_lmk(const uint8_t mac[6], const uint8_t lmk[16]);

/*
 * Data path - two-phase so the AT layer can emit "OK" once the frame is
 * queued and the +ENSENDOK/+ENSENDFAIL URC once the send callback fires:
 *
 *   en_send_start()  queue one frame        -> EN_OK or error code
 *   en_send_finish() wait for send callback -> EN_OK (delivered) or
 *                                              EN_ERR_UNREACHABLE
 *
 * en_frag_send() blocks through all fragments (fail-fast) and emits
 * +ENFRAGRECV progress URCs itself when enabled.
 */
int  en_send_start(const uint8_t mac[6], const uint8_t *data, size_t len);
int  en_bcast_start(const uint8_t *data, size_t len);
int  en_send_finish(void);
int  en_frag_send(const uint8_t mac[6], const uint8_t *data, size_t total_len);

/* Largest payload accepted by en_send_start()/en_bcast_start(). */
size_t en_max_payload(void);
/* Largest payload accepted by en_frag_send(). */
size_t en_max_frag_payload(void);

/*
 * Broadcast a discovery probe and collect responses for timeout_ms,
 * then call cb once per unique responder (rssi is 0 on ESP8285).
 * Blocking; peers running this firmware answer automatically when
 * built with EN_DISCOVERY_RESPOND.
 */
int  en_discover(int timeout_ms, void (*cb)(const uint8_t mac[6], int rssi));

/* Diagnostics */
int  en_peer_check(const uint8_t mac[6], int *rtt_ms);
void en_get_stats(en_stats_t *out);
/* Return true and fill *rssi if a frame has been seen (any / per peer). */
bool en_get_rssi_last(int *rssi);
bool en_get_rssi_peer(const uint8_t mac[6], int *rssi);

/*
 * en_at.c - ESP-NOW AT command handlers and registration.
 *
 * The reusable line-assembly/dispatch machinery lives in components/at_core
 * (at_parser). This file owns only the ESP-NOW command surface: the AT+EN...
 * handlers, their dispatch table, and the engine config (error-URC prefix
 * "+ENERR", code space, line length) that the ESP-NOW personality feeds to
 * the shared engine. A second AT personality mirrors this file.
 *
 * Handler return convention (see at_parser.h):
 *   AT_R_OK    - engine prints "OK"
 *   AT_R_DONE  - handler already printed its own terminal response
 *   1..99      - engine prints "+ENERR:<n>" then "ERROR"
 *   AT_R_ERROR - engine prints plain "ERROR"
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "at_uart.h"
#include "at_parser.h"
#include "en_at_config.h"
#include "en_at.h"
#include "en_core.h"

/* Plain "ERROR" with no "+ENERR" line == the ESP-NOW generic code. */
#define AT_R_ERROR  EN_ERR_GENERIC

/* ---- lifecycle & configuration ---------------------------------------- */

static int cmd_init(at_type_t type, char *args)
{
    unsigned ch;
    if (type != AT_SET || !at_parse_uint(args, &ch)) {
        return AT_R_ERROR;
    }
    return en_init((int)ch);
}

static int cmd_deinit(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_EXEC) {
        return AT_R_ERROR;
    }
    return en_deinit();
}

static int cmd_ver(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_QUERY) {
        return AT_R_ERROR;
    }
    at_uart_write_line("+ENVER:%s,%lu",
                       EN_FW_VERSION, (unsigned long)en_espnow_version());
    return AT_R_OK;
}

/*
 * LTE-modem-style identity commands (3GPP TS 27.007). Each is an
 * execution command answering with a bare identity string then OK, the
 * way a cellular modem reports +CGMI/+CGMM/+CGMR.
 */
static int cmd_cgmi(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_EXEC) {
        return AT_R_ERROR;
    }
    at_uart_write_line("%s", EN_MANUFACTURER);
    return AT_R_OK;
}

static int cmd_cgmm(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_EXEC) {
        return AT_R_ERROR;
    }
    at_uart_write_line("%s", EN_MODEL);
    return AT_R_OK;
}

static int cmd_cgmr(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_EXEC) {
        return AT_R_ERROR;
    }
    at_uart_write_line("%s", EN_FW_VERSION);
    return AT_R_OK;
}

static int cmd_channel(at_type_t type, char *args)
{
    if (type == AT_QUERY) {
        at_uart_write_line("+ENCHANNEL:%d", en_get_channel());
        return AT_R_OK;
    }
    if (type == AT_SET) {
        unsigned ch;
        if (!at_parse_uint(args, &ch)) {
            return AT_R_ERROR;
        }
        return en_set_channel((int)ch);
    }
    return AT_R_ERROR;
}

static int cmd_rate(at_type_t type, char *args)
{
    unsigned idx;
    if (type != AT_SET || !at_parse_uint(args, &idx)) {
        return AT_R_ERROR;
    }
    return en_set_rate((int)idx);
}

/* Standard rates only, capped at 921600 baud. */
static const int s_baud_rates[] = {
    1200, 2400, 4800, 9600, 19200, 38400, 57600,
    115200, 230400, 460800, 921600,
};

static int cmd_baud(at_type_t type, char *args)
{
    if (type == AT_QUERY) {
        at_uart_write_line("+ENBAUD:%d", at_uart_get_baud());
        return AT_R_OK;
    }
    if (type != AT_SET) {
        return AT_R_ERROR;
    }

    unsigned baud;
    if (!at_parse_uint(args, &baud)) {
        return AT_R_ERROR;
    }
    bool valid = false;
    for (size_t i = 0; i < sizeof(s_baud_rates) / sizeof(s_baud_rates[0]); i++) {
        if ((unsigned)s_baud_rates[i] == baud) {
            valid = true;
            break;
        }
    }
    if (!valid) {
        return AT_R_ERROR;
    }

    /* Acknowledge at the current rate; the switch drains TX first so
     * the OK still goes out before the rate changes. */
    at_resp_ok();
    at_uart_set_baud((int)baud);
    return AT_R_DONE;
}

static int cmd_flow(at_type_t type, char *args)
{
    if (type == AT_QUERY) {
        at_uart_write_line("+ENFLOW:%d", at_uart_get_flowctrl());
        return AT_R_OK;
    }
    if (type != AT_SET) {
        return AT_R_ERROR;
    }

    unsigned mode;
    if (!at_parse_uint(args, &mode) || mode > AT_UART_FLOWCTRL_CTS_RTS) {
        return AT_R_ERROR;
    }

#if AT_TARGET_ESP8266 && AT_UART_SWAP_IO
    /* The UART0 IO swap steals the RTS/CTS GPIOs for TX/RX, so hardware
     * flow control cannot be enabled on this build (see at_core_config.h). */
    if (mode != AT_UART_FLOWCTRL_NONE) {
        return AT_R_ERROR;
    }
#endif

    /* Acknowledge at the current flow-control state, then switch: the
     * switch drains TX first so the OK still leaves even when enabling
     * CTS would otherwise gate this device's transmitter (spec 1: the
     * host reconfigures its own side after seeing OK). */
    at_resp_ok();
    at_uart_set_flowctrl((int)mode);
    return AT_R_DONE;
}

static int cmd_mac(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_QUERY) {
        return AT_R_ERROR;
    }
    uint8_t mac[6];
    char macs[13];
    en_get_mac(mac);
    at_mac_str(mac, macs);
    at_uart_write_line("+ENMAC:%s", macs);
    return AT_R_OK;
}

/* ---- peer management --------------------------------------------------- */

static int cmd_addpeer(at_type_t type, char *args)
{
    char *f[4];
    int n = at_split_args(args, f, 4);
    if (type != AT_SET || n < 3) {
        return AT_R_ERROR;
    }

    uint8_t mac[6];
    unsigned ch, enc;
    uint8_t lmk[16];
    bool have_lmk = false;

    if (!at_parse_mac(f[0], mac) || !at_parse_uint(f[1], &ch) ||
        !at_parse_uint(f[2], &enc) || enc > 1) {
        return AT_R_ERROR;
    }
    if (n == 4) {
        if (strlen(f[3]) != 32 || !at_hex_decode(f[3], lmk, 16)) {
            return EN_ERR_BAD_KEY;
        }
        have_lmk = true;
    }
    return en_add_peer(mac, (int)ch, enc == 1, have_lmk ? lmk : NULL);
}

static int cmd_delpeer(at_type_t type, char *args)
{
    uint8_t mac[6];
    if (type != AT_SET || !at_parse_mac(args, mac)) {
        return AT_R_ERROR;
    }
    return en_del_peer(mac);
}

static void list_peer_cb(const uint8_t mac[6], int channel, bool encrypt)
{
    char macs[13];
    at_mac_str(mac, macs);
    at_uart_write_line("+ENLISTPEER:%s,%d,%d", macs, channel, encrypt ? 1 : 0);
}

static int cmd_listpeer(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_QUERY) {
        return AT_R_ERROR;
    }
    return en_list_peers(list_peer_cb);
}

static int cmd_pmk(at_type_t type, char *args)
{
    uint8_t pmk[16];
    if (type != AT_SET || strlen(args) != 32 || !at_hex_decode(args, pmk, 16)) {
        return EN_ERR_BAD_KEY;
    }
    return en_set_pmk(pmk);
}

static int cmd_lmk(at_type_t type, char *args)
{
    char *f[2];
    uint8_t mac[6], lmk[16];
    if (type != AT_SET || at_split_args(args, f, 2) != 2 ||
        !at_parse_mac(f[0], mac) ||
        strlen(f[1]) != 32 || !at_hex_decode(f[1], lmk, 16)) {
        return EN_ERR_BAD_KEY;
    }
    return en_set_lmk(mac, lmk);
}

/* ---- data path ----------------------------------------------------------- */

/*
 * Common tail for all single-frame sends: emit OK once queued, then the
 * +ENSENDOK/+ENSENDFAIL URC once the send callback fires.
 */
static int send_finish_urc(const char *macs)
{
    at_resp_ok();
    if (en_send_finish() == EN_OK) {
        at_uart_write_line("+ENSENDOK:%s", macs);
    } else {
        at_uart_write_line("+ENSENDFAIL:%s", macs);
    }
    return AT_R_DONE;
}

static int cmd_send(at_type_t type, char *args)
{
    char *f[3];
    if (type != AT_SET || at_split_args(args, f, 3) != 3) {
        return AT_R_ERROR;
    }

    uint8_t mac[6];
    unsigned len;
    if (!at_parse_mac(f[0], mac) || !at_parse_uint(f[1], &len)) {
        return AT_R_ERROR;
    }
    if (len == 0 || len > en_max_payload()) {
        return EN_ERR_TOO_LARGE;
    }
    if (strlen(f[2]) != 2 * len) {
        return AT_R_ERROR;
    }

    uint8_t *payload = malloc(len);
    if (!payload || !at_hex_decode(f[2], payload, len)) {
        free(payload);
        return AT_R_ERROR;
    }

    int r = en_send_start(mac, payload, len);
    free(payload);
    if (r != EN_OK) {
        return r;
    }
    return send_finish_urc(f[0]);
}

static int cmd_sendraw(at_type_t type, char *args)
{
    char *f[2];
    if (type != AT_SET || at_split_args(args, f, 2) != 2) {
        return AT_R_ERROR;
    }

    uint8_t mac[6];
    unsigned len;
    if (!at_parse_mac(f[0], mac) || !at_parse_uint(f[1], &len)) {
        return AT_R_ERROR;
    }
    if (len == 0 || len > en_max_payload()) {
        return EN_ERR_TOO_LARGE;
    }
    if (!en_is_init()) {
        return EN_ERR_NOT_INIT;
    }

    /* Prompt for the raw bytes, then collect exactly <len> of them. */
    at_uart_write("\r\n> ", 4);

    uint8_t *payload = malloc(len);
    if (!payload) {
        return AT_R_ERROR;
    }
    size_t got = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(EN_RAW_DATA_TIMEOUT_MS);
    while (got < len) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) {
            break;
        }
        int n = at_uart_read(payload + got, len - got, deadline - now);
        got += (size_t)n;
    }
    if (got < len) {
        free(payload);
        return AT_R_ERROR;     /* host never delivered the full payload */
    }

    int r = en_send_start(mac, payload, len);
    free(payload);
    if (r != EN_OK) {
        return r;
    }
    return send_finish_urc(f[0]);
}

static int cmd_fragsend(at_type_t type, char *args)
{
    char *f[3];
    if (type != AT_SET || at_split_args(args, f, 3) != 3) {
        return AT_R_ERROR;
    }

    uint8_t mac[6];
    unsigned len;
    if (!at_parse_mac(f[0], mac) || !at_parse_uint(f[1], &len)) {
        return AT_R_ERROR;
    }
    if (len == 0 || len > en_max_frag_payload()) {
        return EN_ERR_TOO_LARGE;
    }
    if (strlen(f[2]) != 2 * len) {
        return AT_R_ERROR;
    }
    if (!en_is_init()) {
        return EN_ERR_NOT_INIT;
    }

    uint8_t *payload = malloc(len);
    if (!payload || !at_hex_decode(f[2], payload, len)) {
        free(payload);
        return AT_R_ERROR;
    }

    /* OK acknowledges the command; the URC reports delivery of ALL
     * fragments (fail-fast, spec 4.4). */
    at_resp_ok();
    int r = en_frag_send(mac, payload, len);
    free(payload);
    if (r == EN_OK) {
        at_uart_write_line("+ENSENDOK:%s", f[0]);
    } else {
        at_uart_write_line("+ENSENDFAIL:%s", f[0]);
    }
    return AT_R_DONE;
}

static int cmd_bcast(at_type_t type, char *args)
{
    char *f[2];
    if (type != AT_SET || at_split_args(args, f, 2) != 2) {
        return AT_R_ERROR;
    }

    unsigned len;
    if (!at_parse_uint(f[0], &len)) {
        return AT_R_ERROR;
    }
    if (len == 0 || len > en_max_payload()) {
        return EN_ERR_TOO_LARGE;
    }
    if (strlen(f[1]) != 2 * len) {
        return AT_R_ERROR;
    }

    uint8_t *payload = malloc(len);
    if (!payload || !at_hex_decode(f[1], payload, len)) {
        free(payload);
        return AT_R_ERROR;
    }

    int r = en_bcast_start(payload, len);
    free(payload);
    if (r != EN_OK) {
        return r;
    }
    return send_finish_urc("FFFFFFFFFFFF");
}

/* ---- diagnostics ---------------------------------------------------------- */

static int cmd_rssi(at_type_t type, char *args)
{
    int rssi;
    if (type == AT_QUERY) {
        if (!en_get_rssi_last(&rssi)) {
            return AT_R_ERROR;      /* nothing received yet */
        }
        at_uart_write_line("+ENRSSI:%d", rssi);
        return AT_R_OK;
    }
    if (type == AT_SET) {
        uint8_t mac[6];
        if (!at_parse_mac(args, mac)) {
            return AT_R_ERROR;
        }
        if (!en_get_rssi_peer(mac, &rssi)) {
            return AT_R_ERROR;      /* no frame from that peer yet */
        }
        at_uart_write_line("+ENRSSI:%s,%d", args, rssi);
        return AT_R_OK;
    }
    return AT_R_ERROR;
}

static int cmd_stats(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_QUERY) {
        return AT_R_ERROR;
    }
    en_stats_t st;
    en_get_stats(&st);
    at_uart_write_line("+ENSTATS:%lu,%lu,%lu,%lu",
                       (unsigned long)st.tx_ok, (unsigned long)st.tx_fail,
                       (unsigned long)st.rx_ok, (unsigned long)st.rx_drop);
    return AT_R_OK;
}

static int cmd_state(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_QUERY) {
        return AT_R_ERROR;
    }
    at_uart_write_line("+ENSTATE:%d", (int)en_get_state());
    return AT_R_OK;
}

static void discover_line_cb(const uint8_t mac[6], int rssi)
{
    char macs[13];
    at_mac_str(mac, macs);
    at_uart_write_line("+ENDISCOVER:%s,%d", macs, rssi);
}

static int cmd_discover(at_type_t type, char *args)
{
    int timeout_ms = EN_DISCOVER_TIMEOUT_MS;
    if (type == AT_SET) {
        unsigned t;
        if (!at_parse_uint(args, &t) || t < 50 || t > 30000) {
            return AT_R_ERROR;
        }
        timeout_ms = (int)t;
    } else if (type != AT_EXEC) {
        return AT_R_ERROR;
    }
    return en_discover(timeout_ms, discover_line_cb);
}

static int cmd_discoverable(at_type_t type, char *args)
{
    if (type == AT_QUERY) {
        at_uart_write_line("+ENDISCOVERABLE:%d", en_get_discoverable() ? 1 : 0);
        return AT_R_OK;
    }
    if (type == AT_SET) {
        unsigned on;
        if (!at_parse_uint(args, &on) || on > 1) {
            return AT_R_ERROR;
        }
        en_set_discoverable(on == 1);
        return AT_R_OK;
    }
    return AT_R_ERROR;
}

static int cmd_peercheck(at_type_t type, char *args)
{
    uint8_t mac[6];
    if (type != AT_SET || !at_parse_mac(args, mac)) {
        return AT_R_ERROR;
    }
    int rtt_ms = 0;
    int r = en_peer_check(mac, &rtt_ms);
    if (r != EN_OK) {
        return r;
    }
    at_uart_write_line("+ENPEERCHECK:%s,%d", args, rtt_ms);
    return AT_R_OK;
}

/* ---- dispatch table & registration ---------------------------------------- */

static const at_command_t s_cmds[] = {
    { "ENINIT",      cmd_init      },
    { "ENDEINIT",    cmd_deinit    },
    { "ENVER",       cmd_ver       },
    { "CGMI",        cmd_cgmi      },
    { "CGMM",        cmd_cgmm      },
    { "CGMR",        cmd_cgmr      },
    { "ENCHANNEL",   cmd_channel   },
    { "ENRATE",      cmd_rate      },
    { "ENBAUD",      cmd_baud      },
    { "ENFLOW",      cmd_flow      },
    { "ENMAC",       cmd_mac       },
    { "ENADDPEER",   cmd_addpeer   },
    { "ENDELPEER",   cmd_delpeer   },
    { "ENLISTPEER",  cmd_listpeer  },
    { "ENPMK",       cmd_pmk       },
    { "ENLMK",       cmd_lmk       },
    { "ENSENDRAW",   cmd_sendraw   },
    { "ENSEND",      cmd_send      },
    { "ENFRAGSEND",  cmd_fragsend  },
    { "ENBCAST",     cmd_bcast     },
    { "ENRSSI",      cmd_rssi      },
    { "ENSTATS",     cmd_stats     },
    { "ENSTATE",     cmd_state     },
    { "ENPEERCHECK", cmd_peercheck },
    { "ENDISCOVERABLE", cmd_discoverable },
    { "ENDISCOVER",  cmd_discover  },
};

/* Engine config for the ESP-NOW personality: "+ENERR" code space, the
 * ESP-NOW line length, and the parser task tuning. */
const at_engine_cfg_t en_at_engine_cfg = {
    .line_max        = EN_AT_LINE_MAX,
    .err_prefix      = "+ENERR",
    .err_generic     = EN_ERR_GENERIC,
    .err_unsupported = EN_ERR_UNSUPPORTED,
    .echo_default    = EN_AT_ECHO_DEFAULT,
    .task_stack      = EN_PARSER_TASK_STACK,
    .task_prio       = EN_PARSER_TASK_PRIO,
};

void en_at_register(void)
{
    at_register_commands(s_cmds, sizeof(s_cmds) / sizeof(s_cmds[0]));
}

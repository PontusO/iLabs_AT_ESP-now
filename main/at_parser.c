/*
 * at_parser.c - AT command line assembly and dispatch.
 *
 * Grammar (spec section 1):
 *   AT+CMD?          query
 *   AT+CMD=<params>  set
 *   AT+CMD           execute
 *
 * Terminal responses are "OK" or "ERROR"; specific faults additionally
 * emit "+ENERR:<code>" on the line before "ERROR". URCs may be
 * interleaved at any time by the RX worker (at_uart serializes lines).
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "en_at_config.h"
#include "at_uart.h"
#include "at_parser.h"
#include "en_core.h"

static const char *TAG = "at_parser";

typedef enum {
    AT_EXEC,    /* AT+CMD          */
    AT_QUERY,   /* AT+CMD?         */
    AT_SET,     /* AT+CMD=<params> */
} at_type_t;

/*
 * Handler return convention:
 *   AT_R_OK    - parser prints "OK"
 *   AT_R_DONE  - handler already printed its own terminal response
 *   1..99      - parser prints "+ENERR:<n>" then "ERROR"
 *   AT_R_ERROR - parser prints plain "ERROR"
 */
#define AT_R_OK     0
#define AT_R_DONE   (-1)
#define AT_R_ERROR  EN_ERR_GENERIC

typedef int (*at_handler_t)(at_type_t type, char *args);

typedef struct {
    const char  *name;      /* without the "AT+" prefix */
    at_handler_t handler;
} at_cmd_t;

static bool s_echo = EN_AT_ECHO_DEFAULT;

/* ---- response helpers ------------------------------------------------ */

static void resp_ok(void)
{
    at_uart_write_line("OK");
}

static void resp_error(void)
{
    at_uart_write_line("ERROR");
}

static void resp_err_code(int code)
{
    at_uart_write_line("+ENERR:%d", code);
    resp_error();
}

/* ---- parse helpers ---------------------------------------------------- */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Decode exactly 2*out_len hex chars into out. */
static bool hex_decode(const char *hex, uint8_t *out, size_t out_len)
{
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* MAC = exactly 12 hex chars, no separators. */
static bool parse_mac(const char *s, uint8_t mac[6])
{
    return strlen(s) == 12 && hex_decode(s, mac, 6);
}

static bool parse_uint(const char *s, unsigned *out)
{
    if (*s == '\0') {
        return false;
    }
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*end != '\0') {
        return false;
    }
    *out = (unsigned)v;
    return true;
}

/*
 * Split args on commas, in place. Returns the number of fields
 * (up to max), or -1 if there are more fields than max.
 */
static int split_args(char *args, char *fields[], int max)
{
    int n = 0;
    if (!args || *args == '\0') {
        return 0;
    }
    char *p = args;
    for (;;) {
        if (n == max) {
            return -1;
        }
        fields[n++] = p;
        char *comma = strchr(p, ',');
        if (!comma) {
            return n;
        }
        *comma = '\0';
        p = comma + 1;
    }
}

static void mac_str(const uint8_t mac[6], char out[13])
{
    snprintf(out, 13, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ---- lifecycle & configuration ---------------------------------------- */

static int cmd_init(at_type_t type, char *args)
{
    unsigned ch;
    if (type != AT_SET || !parse_uint(args, &ch)) {
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

static int cmd_channel(at_type_t type, char *args)
{
    if (type == AT_QUERY) {
        at_uart_write_line("+ENCHANNEL:%d", en_get_channel());
        return AT_R_OK;
    }
    if (type == AT_SET) {
        unsigned ch;
        if (!parse_uint(args, &ch)) {
            return AT_R_ERROR;
        }
        return en_set_channel((int)ch);
    }
    return AT_R_ERROR;
}

static int cmd_rate(at_type_t type, char *args)
{
    unsigned idx;
    if (type != AT_SET || !parse_uint(args, &idx)) {
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
    if (!parse_uint(args, &baud)) {
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
    resp_ok();
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
    if (!parse_uint(args, &mode) || mode > EN_UART_FLOWCTRL_CTS_RTS) {
        return AT_R_ERROR;
    }

#if EN_TARGET_ESP8266 && EN_UART_SWAP_IO
    /* The UART0 IO swap steals the RTS/CTS GPIOs for TX/RX, so hardware
     * flow control cannot be enabled on this build (see en_at_config.h). */
    if (mode != EN_UART_FLOWCTRL_NONE) {
        return AT_R_ERROR;
    }
#endif

    /* Acknowledge at the current flow-control state, then switch: the
     * switch drains TX first so the OK still leaves even when enabling
     * CTS would otherwise gate this device's transmitter (spec 1: the
     * host reconfigures its own side after seeing OK). */
    resp_ok();
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
    mac_str(mac, macs);
    at_uart_write_line("+ENMAC:%s", macs);
    return AT_R_OK;
}

/* ---- peer management --------------------------------------------------- */

static int cmd_addpeer(at_type_t type, char *args)
{
    char *f[4];
    int n = split_args(args, f, 4);
    if (type != AT_SET || n < 3) {
        return AT_R_ERROR;
    }

    uint8_t mac[6];
    unsigned ch, enc;
    uint8_t lmk[16];
    bool have_lmk = false;

    if (!parse_mac(f[0], mac) || !parse_uint(f[1], &ch) ||
        !parse_uint(f[2], &enc) || enc > 1) {
        return AT_R_ERROR;
    }
    if (n == 4) {
        if (strlen(f[3]) != 32 || !hex_decode(f[3], lmk, 16)) {
            return EN_ERR_BAD_KEY;
        }
        have_lmk = true;
    }
    return en_add_peer(mac, (int)ch, enc == 1, have_lmk ? lmk : NULL);
}

static int cmd_delpeer(at_type_t type, char *args)
{
    uint8_t mac[6];
    if (type != AT_SET || !parse_mac(args, mac)) {
        return AT_R_ERROR;
    }
    return en_del_peer(mac);
}

static void list_peer_cb(const uint8_t mac[6], int channel, bool encrypt)
{
    char macs[13];
    mac_str(mac, macs);
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
    if (type != AT_SET || strlen(args) != 32 || !hex_decode(args, pmk, 16)) {
        return EN_ERR_BAD_KEY;
    }
    return en_set_pmk(pmk);
}

static int cmd_lmk(at_type_t type, char *args)
{
    char *f[2];
    uint8_t mac[6], lmk[16];
    if (type != AT_SET || split_args(args, f, 2) != 2 ||
        !parse_mac(f[0], mac) ||
        strlen(f[1]) != 32 || !hex_decode(f[1], lmk, 16)) {
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
    resp_ok();
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
    if (type != AT_SET || split_args(args, f, 3) != 3) {
        return AT_R_ERROR;
    }

    uint8_t mac[6];
    unsigned len;
    if (!parse_mac(f[0], mac) || !parse_uint(f[1], &len)) {
        return AT_R_ERROR;
    }
    if (len == 0 || len > en_max_payload()) {
        return EN_ERR_TOO_LARGE;
    }
    if (strlen(f[2]) != 2 * len) {
        return AT_R_ERROR;
    }

    uint8_t *payload = malloc(len);
    if (!payload || !hex_decode(f[2], payload, len)) {
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
    if (type != AT_SET || split_args(args, f, 2) != 2) {
        return AT_R_ERROR;
    }

    uint8_t mac[6];
    unsigned len;
    if (!parse_mac(f[0], mac) || !parse_uint(f[1], &len)) {
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
    if (type != AT_SET || split_args(args, f, 3) != 3) {
        return AT_R_ERROR;
    }

    uint8_t mac[6];
    unsigned len;
    if (!parse_mac(f[0], mac) || !parse_uint(f[1], &len)) {
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
    if (!payload || !hex_decode(f[2], payload, len)) {
        free(payload);
        return AT_R_ERROR;
    }

    /* OK acknowledges the command; the URC reports delivery of ALL
     * fragments (fail-fast, spec 4.4). */
    resp_ok();
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
    if (type != AT_SET || split_args(args, f, 2) != 2) {
        return AT_R_ERROR;
    }

    unsigned len;
    if (!parse_uint(f[0], &len)) {
        return AT_R_ERROR;
    }
    if (len == 0 || len > en_max_payload()) {
        return EN_ERR_TOO_LARGE;
    }
    if (strlen(f[1]) != 2 * len) {
        return AT_R_ERROR;
    }

    uint8_t *payload = malloc(len);
    if (!payload || !hex_decode(f[1], payload, len)) {
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
        if (!parse_mac(args, mac)) {
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
    mac_str(mac, macs);
    at_uart_write_line("+ENDISCOVER:%s,%d", macs, rssi);
}

static int cmd_discover(at_type_t type, char *args)
{
    int timeout_ms = EN_DISCOVER_TIMEOUT_MS;
    if (type == AT_SET) {
        unsigned t;
        if (!parse_uint(args, &t) || t < 50 || t > 30000) {
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
        if (!parse_uint(args, &on) || on > 1) {
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
    if (type != AT_SET || !parse_mac(args, mac)) {
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

/* ---- dispatch --------------------------------------------------------------- */

static const at_cmd_t s_cmds[] = {
    { "ENINIT",      cmd_init      },
    { "ENDEINIT",    cmd_deinit    },
    { "ENVER",       cmd_ver       },
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

static void handle_result(int r)
{
    if (r == AT_R_OK) {
        resp_ok();
    } else if (r == AT_R_DONE) {
        /* handler already answered */
    } else if (r > 0 && r < EN_ERR_GENERIC) {
        resp_err_code(r);
    } else {
        resp_error();
    }
}

static void process_line(char *line)
{
    /* Bare "AT" attention poke. */
    if (strcasecmp(line, "AT") == 0) {
        resp_ok();
        return;
    }
    /* Echo control, standard V.250. */
    if (strcasecmp(line, "ATE0") == 0) {
        s_echo = false;
        resp_ok();
        return;
    }
    if (strcasecmp(line, "ATE1") == 0) {
        s_echo = true;
        resp_ok();
        return;
    }

    if (strncasecmp(line, "AT+", 3) != 0) {
        resp_error();
        return;
    }

    char *cmd = line + 3;
    at_type_t type = AT_EXEC;
    char *args = NULL;

    char *sep = strpbrk(cmd, "=?");
    if (sep && *sep == '?') {
        if (*(sep + 1) != '\0') {
            resp_error();
            return;
        }
        type = AT_QUERY;
        *sep = '\0';
    } else if (sep && *sep == '=') {
        type = AT_SET;
        *sep = '\0';
        args = sep + 1;
    }

    for (size_t i = 0; i < sizeof(s_cmds) / sizeof(s_cmds[0]); i++) {
        if (strcasecmp(cmd, s_cmds[i].name) == 0) {
            handle_result(s_cmds[i].handler(type, args));
            return;
        }
    }

    /* Unknown AT+ command: distinct code so hosts can detect version
     * skew (spec section 7, "version skew"). */
    resp_err_code(EN_ERR_UNSUPPORTED);
}

/* ---- parser task --------------------------------------------------------------- */

static void parser_task(void *arg)
{
    (void)arg;

    char *line = malloc(EN_AT_LINE_MAX);
    configASSERT(line);
    size_t pos = 0;
    bool overflow = false;

    uint8_t chunk[64];

    for (;;) {
        /* Block for the first byte, then drain whatever else is already
         * buffered (uart_read_bytes waits for the FULL length otherwise). */
        int n = at_uart_read(chunk, 1, portMAX_DELAY);
        if (n == 1) {
            n += at_uart_read(chunk + 1, sizeof(chunk) - 1, 0);
        }
        for (int i = 0; i < n; i++) {
            char c = (char)chunk[i];

            if (c == '\r' || c == '\n') {
                if (s_echo) {
                    at_uart_write("\r\n", 2);
                }
                if (overflow) {
                    resp_error();
                } else if (pos > 0) {
                    line[pos] = '\0';
                    process_line(line);
                }
                pos = 0;
                overflow = false;
                continue;
            }

            /* Minimal line editing for interactive bring-up. */
            if (c == '\b' || c == 0x7F) {
                if (pos > 0) {
                    pos--;
                    if (s_echo) {
                        at_uart_write("\b \b", 3);
                    }
                }
                continue;
            }

            if (pos < EN_AT_LINE_MAX - 1) {
                line[pos++] = c;
                if (s_echo) {
                    at_uart_write(&c, 1);
                }
            } else {
                overflow = true;
            }
        }
    }
}

void at_parser_start(void)
{
    BaseType_t ok = xTaskCreate(parser_task, "at_parser",
                                EN_PARSER_TASK_STACK, NULL,
                                EN_PARSER_TASK_PRIO, NULL);
    configASSERT(ok == pdPASS);
    ESP_LOGI(TAG, "parser started");
}

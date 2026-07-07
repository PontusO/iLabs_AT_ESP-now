# ESP-NOW AT Interpreter (`AT+EN`)

AT command interpreter exposing ESP-NOW peer management, data transfer and
diagnostics over a UART link, for use as an ESP32 slave co-processor next to a
host MCU (RP2040 / RP2350 / nRF52840) — iLabs Challenger+ platform.

Implements the **AT+EN Command Set Specification v0.1** on ESP-IDF for
**ESP32-C6** and **ESP32-C3**, and on ESP8266_RTOS_SDK for the
**ESP8285** (2 MB embedded flash) — see
[ESP8285 target notes](#esp8285-target-notes) for that chip's limitations.

```
Host MCU (RP2040/nRF52840)          ESP32-C3/C6 slave
        |                                  |
        |  UART (optional RTS/CTS)         |
        | AT+ENSEND=... ------------------>|            ESP-NOW
        |<------------------------- OK     | ))) 2.4 GHz ((( peer boards
        |<---------------- +ENSENDOK:...   |
        |<---------------- +ENRECV:...     |
```

## Features

- Full `AT+EN` command set: lifecycle, peer management, keys (PMK/LMK),
  single-frame send, raw binary passthrough, fragmented send/reassembly,
  broadcast, RSSI, statistics, state and peer liveness probe.
- Configurable UART transport: port, pins, baud rate and hardware flow
  control (none / RTS / CTS / RTS+CTS) — all in **one global config file**.
- URC-driven receive path (`+ENRECV`) with per-fragment progress URCs.
- Encrypted peers via ESP-NOW PMK/LMK.
- Runs on ESP32-C6 and ESP32-C3 (any ESP-IDF v5.x target should work),
  plus ESP8285/ESP8266 via ESP8266_RTOS_SDK v3.4.

## Repository layout

```
CMakeLists.txt              ESP-IDF project file
sdkconfig.defaults          shared build defaults (C3 + C6)
sdkconfig.defaults.esp8285  self-contained defaults for ESP8285 (2 MB, DOUT)
main/
  main.c                    boot sequence
  at_uart.c                 UART transport (thread-safe TX, flow control)
  at_parser.c               AT line assembly, grammar, command handlers
  en_core.c                 ESP-NOW engine (peers, keys, data path, frag, ping)
  include/
    en_at_config.h          <-- global configuration: pins, UART, tunables
    at_uart.h / at_parser.h / en_core.h
```

## Configuration — `main/include/en_at_config.h`

Everything board-specific lives in this single header. Edit and rebuild.

| Macro | Default | Meaning |
|---|---|---|
| `EN_UART_PORT` | `UART_NUM_1` | UART used for the AT link (keep the console on UART0) |
| `EN_UART_BAUD` | `115200` | AT link baud rate |
| `EN_UART_FLOWCTRL` | `EN_UART_FLOWCTRL_NONE` | `NONE`, `RTS`, `CTS` or `CTS_RTS` |
| `EN_UART_RTS_THRESH` | `100` | RX FIFO level that de-asserts RTS |
| `EN_UART_TX_PIN` / `EN_UART_RX_PIN` | C6: 5/4, C3: 7/6 | AT UART data pins |
| `EN_UART_RTS_PIN` / `EN_UART_CTS_PIN` | C6: 6/7, C3: 5/4 | flow control pins (`EN_PIN_NC` if unused) |
| `EN_FRAG_MAX_TOTAL` | `4096` | max `AT+ENFRAGSEND` payload / reassembly buffer |
| `EN_FRAG_RX_SLOTS` | `4` | concurrent fragmented-receive sources |
| `EN_SEND_CB_TIMEOUT_MS` | `1000` | max wait for the ESP-NOW send callback |
| `EN_PING_TIMEOUT_MS` | `500` | `AT+ENPEERCHECK` round-trip timeout |
| `EN_FRAG_TIMEOUT_MS` | `3000` | reassembly timeout (stale transfers dropped) |
| `EN_RAW_DATA_TIMEOUT_MS` | `10000` | `AT+ENSENDRAW` wait for raw bytes |
| `EN_AT_ECHO_DEFAULT` | `0` | command echo at boot (`ATE0`/`ATE1` at runtime) |
| `EN_URC_FRAG_PROGRESS` | `1` | emit `+ENFRAGRECV` progress URCs |
| `EN_URC_READY_ON_BOOT` | `1` | emit `+ENREADY` once after reset |

On ESP32-C3/C6 all four UART pins go through the GPIO matrix, so any free
GPIO may be used. Pin defaults are per-target (`CONFIG_IDF_TARGET_*` blocks
in the same file). On the ESP8285 the UART0 pins are fixed by the chip's IO
mux (TX=GPIO1, RX=GPIO3, RTS=GPIO15, CTS=GPIO13); the only pin option there
is `EN_UART_SWAP_IO`, which moves UART0 to GPIO15(TX)/GPIO13(RX).

> **Note:** the AT link defaults to UART1 so that ESP-IDF boot/log output on
> UART0 can never corrupt the AT stream. If you move the AT link to UART0,
> also silence the console log (`CONFIG_LOG_DEFAULT_LEVEL_NONE`).

## Building

Prerequisites: [ESP-IDF v5.1 or later](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/get-started/index.html)
(developed and tested against v5.3), with the RISC-V toolchain installed
(`./install.sh esp32c3,esp32c6`).

```sh
. $IDF_PATH/export.sh

# ESP32-C6
idf.py set-target esp32c6
idf.py build

# ESP32-C3
idf.py set-target esp32c3
idf.py build

# flash + monitor (adjust the port)
idf.py -p /dev/ttyUSB0 flash monitor
```

`idf.py set-target` regenerates `sdkconfig` from `sdkconfig.defaults`; switch
targets at any time the same way.

### Building for ESP8285 (ESP8266_RTOS_SDK)

The ESP8285 is an ESP8266 with 2 MB flash on-die; it is **not** supported by
ESP-IDF. Install
[ESP8266_RTOS_SDK v3.4](https://github.com/espressif/ESP8266_RTOS_SDK)
and its xtensa-lx106 toolchain, then build the same source tree with the
ESP8285 defaults file (2 MB flash, DOUT mode — mandatory for the on-die
flash — and console moved to UART1):

```sh
export IDF_PATH=~/esp/ESP8266_RTOS_SDK      # adjust
. $IDF_PATH/export.sh

rm -f sdkconfig                              # discard an ESP32 config
idf.py -DSDKCONFIG_DEFAULTS=sdkconfig.defaults.esp8285 build
idf.py -p /dev/ttyUSB0 flash monitor
```

The 8266 SDK accepts only a single defaults file, which is why
`sdkconfig.defaults.esp8285` is self-contained rather than layered on top of
`sdkconfig.defaults`.

## Using it

Wire the host MCU to the AT UART pins (3-wire TX/RX/GND minimum; add RTS/CTS
if you enable flow control), open the port at 115200 8N1 and talk AT:

```
AT
OK
AT+ENMAC?
+ENMAC:AABBCCDDEE01
OK
AT+ENINIT=6
OK
```

Conventions (spec section 1):

- `AT+CMD?` query, `AT+CMD=<params>` set, `AT+CMD` execute.
- Every command terminates with `OK` or `ERROR`; specific faults print
  `+ENERR:<code>` on the preceding line.
- URCs (`+EN...`) can arrive **at any time**, including between a command and
  its terminal response — the host parser must dispatch them out-of-band.
- MAC addresses are 12 hex chars, no separators: `AABBCCDDEEFF`.
- One command in flight at a time; wait for `OK`/`ERROR` before the next
  command (except while streaming payload bytes to `AT+ENSENDRAW`).
- After reset the firmware emits `+ENREADY` so the host can resynchronize.

### Command reference

| Command | Syntax | Purpose |
|---|---|---|
| `AT` | `AT` | attention / link test |
| `ATE0` / `ATE1` | | command echo off / on |
| `AT+ENINIT` | `AT+ENINIT=<channel>` | init WiFi (STA underlay) + ESP-NOW on channel 1–14 |
| `AT+ENDEINIT` | `AT+ENDEINIT` | tear down ESP-NOW and WiFi, free peers |
| `AT+ENVER` | `AT+ENVER?` | `+ENVER:<fw_ver>,<espnow_ver>` |
| `AT+ENCHANNEL` | `AT+ENCHANNEL?` / `=<ch>` | get/set WiFi channel |
| `AT+ENRATE` | `AT+ENRATE=<rate_idx>` | set PHY rate (see below) |
| `AT+ENMAC` | `AT+ENMAC?` | this device's own STA MAC (works before INIT) |
| `AT+ENADDPEER` | `=<mac>,<ch>,<encrypt:0\|1>[,<lmk32hex>]` | register peer (max 20) |
| `AT+ENDELPEER` | `=<mac>` | remove peer |
| `AT+ENLISTPEER` | `AT+ENLISTPEER?` | one `+ENLISTPEER:<mac>,<ch>,<enc>` line per peer |
| `AT+ENPMK` | `=<32 hex chars>` | set Primary Master Key (16 bytes) |
| `AT+ENLMK` | `=<mac>,<32 hex chars>` | set per-peer Local Master Key |
| `AT+ENSEND` | `=<mac>,<len>,<payload_hex>` | send one frame (≤ 248 B, see framing note) |
| `AT+ENSENDRAW` | `=<mac>,<len>` then `>` prompt | binary passthrough send |
| `AT+ENFRAGSEND` | `=<mac>,<total_len>,<payload_hex>` | auto-fragmented send (≤ `EN_FRAG_MAX_TOTAL`) |
| `AT+ENBCAST` | `=<len>,<payload_hex>` | broadcast to all listeners |
| `AT+ENRSSI` | `AT+ENRSSI?` / `=<mac>` | RSSI of last frame (overall / per peer) |
| `AT+ENSTATS` | `AT+ENSTATS?` | `+ENSTATS:<tx_ok>,<tx_fail>,<rx_ok>,<rx_drop>` |
| `AT+ENSTATE` | `AT+ENSTATE?` | `0` uninit, `1` idle, `2` sending, `3` error |
| `AT+ENPEERCHECK` | `=<mac>` | liveness ping: `+ENPEERCHECK:<mac>,<rtt_ms>` |

### URCs (unsolicited result codes)

| URC | Meaning |
|---|---|
| `+ENREADY` | firmware booted / reset |
| `+ENRECV:<src_mac>,<len>,<rssi>,<payload_hex>` | inbound frame (mandatory always-on) |
| `+ENSENDOK:<mac>` / `+ENSENDFAIL:<mac>` | delivery result after `OK` of a send |
| `+ENFRAGRECV:<mac>,<frag_idx>,<frag_total>` | fragment progress (both directions, 1-based) |

### Error codes (`+ENERR:<n>`)

| Code | Meaning |
|---|---|
| 1 | WiFi/stack init failure |
| 2 | Peer table full |
| 3 | Peer unreachable / no ACK |
| 4 | Payload too large for mode (use `AT+ENFRAGSEND`) |
| 5 | Not initialized (command issued before `AT+ENINIT`) |
| 6 | Encryption key invalid/missing |
| 7 | Fragment reassembly timeout |
| 8 | Unknown/unsupported command (version-skew detection) |

### PHY rates (`AT+ENRATE=<rate_idx>`)

`<rate_idx>` is the numeric `wifi_phy_rate_t` value; commonly useful ones:

| idx | rate | idx | rate |
|---|---|---|---|
| 0 | 1 Mbps (11b, max range) | 11 | 6 Mbps (11g) |
| 3 | 11 Mbps (11b) | 12 | 54 Mbps (11g, max throughput) |
| 16 | MCS0 6.5 Mbps (11n) | 23 | MCS7 65 Mbps (11n) |
| 41 | LR 250 kbps (Espressif long-range) | 42 | LR 500 kbps |

The rate applies to all current and future peers. **Long-range (LR) rates
work only between Espressif chips** and both ends must enable them.
`AT+ENRATE` is not available on the ESP8285 (returns `+ENERR:8`).

## Protocol notes (read before integrating)

### Over-the-air framing

Every frame this firmware transmits carries a 2-byte prefix
(`0xEA` magic + frame type) so that data, fragments and the liveness probe
can be told apart on the receive side. Consequences:

- Max `AT+ENSEND` / `AT+ENSENDRAW` / `AT+ENBCAST` payload is **248 bytes**
  (250-byte ESP-NOW limit minus the 2-byte header). Fragments carry a 7-byte
  header (243 payload bytes per fragment).
- Two boards running this firmware interoperate transparently.
- Frames **received** from third-party ESP-NOW senders that don't carry the
  prefix are still delivered verbatim via `+ENRECV`. Frames **sent** to
  third-party receivers will contain the 2-byte prefix — strip it on that
  side, or use spec-compliant firmware on both ends.

### Encryption (PMK/LMK)

- `AT+ENPMK` must be set to the **same value on both boards** before adding
  encrypted peers; the PMK is never exchanged over the air.
- If `AT+ENADDPEER=...,1` is issued **without** an explicit LMK, this
  firmware uses the PMK as that peer's LMK (both sides derive identically, so
  the spec's `PMK → ADDPEER` example works as written). Provide a per-peer
  LMK (4th parameter, or `AT+ENLMK`) for key separation.
- Adding an encrypted peer with **no PMK set and no LMK given** fails with
  `+ENERR:6`.
- ESP-IDF limits encrypted peers (default 7, `CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM`);
  total peers max 20.
- **Security review item (per spec):** keys arrive as plaintext AT parameters
  on the UART. For production, inject them at provisioning time from a
  secure element / secured host flash, not over a sniffable bus. Keys are
  held in RAM only and are **not** persisted to flash by this firmware.

### Persistence

The peer table, PMK/LMK and channel are **not** persisted — the host
re-provisions after every reset (watch for `+ENREADY`). This is a deliberate
choice: it keeps key storage a host-side security decision (spec section 7).

### Fragmentation

- The sender emits `+ENFRAGRECV:<dst>,<n>,<total>` as each fragment is
  acknowledged; the receiver emits the same URC per fragment received, then
  one final `+ENRECV` with the fully reassembled payload.
- Fail-fast: if any fragment is not acknowledged, the sender reports
  `+ENSENDFAIL` immediately and no partial data is delivered.
- A receiver that stops getting fragments drops the transfer silently after
  `EN_FRAG_TIMEOUT_MS` (counts as `rx_drop` in `AT+ENSTATS?`). A host that
  saw some `+ENFRAGRECV` but no final `+ENRECV` should apply its own timeout.

### ESP8285 target notes

The AT surface is identical across all three chips, with these differences
imposed by the ESP8285 hardware and the 8266 SDK:

- **AT link is UART0** (the chip's UART1 is TX-only), on fixed pins:
  TX=GPIO1, RX=GPIO3. Hardware flow control is supported but pin-fixed too:
  RTS=GPIO15, CTS=GPIO13. Console/log output goes to UART1 (TX on GPIO2)
  via `sdkconfig.defaults.esp8285`.
- **Boot ROM chatter:** the mask ROM prints on GPIO1 at 74880 baud during
  every reset — the host must discard input until `+ENREADY`. To keep the
  AT link completely clean, set `EN_UART_SWAP_IO 1` in `en_at_config.h`
  to move UART0 to GPIO15(TX)/GPIO13(RX); this sacrifices hardware flow
  control (same pins).
- **No RSSI:** the 8266 SDK's receive callback does not expose RSSI.
  `+ENRECV` reports the RSSI field as `0`, and `AT+ENRSSI` returns `ERROR`.
- **No PHY rate control:** `AT+ENRATE` returns `+ENERR:8` (unsupported).
- Peer limits differ slightly: 20 total, but at most 6 encrypted peers
  (ESP8266 hardware limit).
- `AT+ENVER?` reports the 8266 SDK's ESP-NOW version, so hosts can key
  feature availability off `AT+ENVER?`/`+ENERR:8` as designed.

### Coexistence warning

If the design must later join a WiFi AP (OTA etc.), ESP-NOW and the AP
**must share the same channel** — `AT+ENCHANNEL` retunes ESP-NOW but cannot
follow an AP's channel automatically.

---

## Use scenarios

*(from the AT+EN specification, section 8)*

### 8.1 First-boot pairing — two boards, MACs exchanged out-of-band

Board A and Board B each need the other's MAC before `ADDPEER` works. Assume
MACs were read once (e.g. at factory test, printed on a label or pushed via
BLE) and are now hardcoded/config-stored on each side.

**Board A** (MAC `AABBCCDDEE01`) pairing to **Board B** (MAC `AABBCCDDEE02`),
channel 6, encrypted:

```
AT+ENINIT=6
OK

AT+ENPMK=000102030405060708090A0B0C0D0E0F
OK

AT+ENADDPEER=AABBCCDDEE02,6,1
OK

AT+ENLISTPEER?
+ENLISTPEER:AABBCCDDEE02,6,1
OK
```

**Board B** runs the mirror image, adding `AABBCCDDEE01` as its peer, same
PMK. Both sides must set the same PMK before either `ADDPEER=...,1,...`
(encrypted) will actually produce interoperable encrypted frames — PMK isn't
exchanged over the air.

Verify the link is alive before trusting it with data:

```
AT+ENPEERCHECK=AABBCCDDEE02
+ENPEERCHECK:AABBCCDDEE02,4
OK
```

If this times out (`+ENERR:3`), check channel match and PMK match on both
sides before anything else — those are the two most common causes of "paired
but silent."

### 8.2 Unencrypted quick-pair (bench/dev use only)

Skips PMK entirely — fine for bring-up on the bench, not for anything
shipping:

```
AT+ENINIT=1
OK
AT+ENADDPEER=AABBCCDDEE02,1,0
OK
```

### 8.3 Send/receive — short telemetry frame

Board A sends a 4-byte sensor reading to Board B, Board B's host sees it
arrive as a URC.

**Board A (sender):**
```
AT+ENSEND=AABBCCDDEE02,4,DEADBEEF
OK
+ENSENDOK:AABBCCDDEE02
```

**Board B (receiver)** — this URC can arrive at any time, independent of what
Board B's host last sent:
```
+ENRECV:AABBCCDDEE01,4,-42,DEADBEEF
```
`-42` is RSSI in dBm. Host on Board B parses payload `DEADBEEF` as its 4-byte
reading.

### 8.4 Bidirectional request/response (command + ack pattern)

Board A asks Board B for a status byte, Board B replies. Since ESP-NOW itself
has no built-in request/response framing, this is an application-layer
convention — e.g. first payload byte as a message type.

**Board A sends request (msg type `0x01` = "status request"):**
```
AT+ENSEND=AABBCCDDEE02,1,01
OK
+ENSENDOK:AABBCCDDEE02
```

**Board B's host sees the request URC, then replies with msg type `0x02` =
"status response" + 1 status byte:**
```
+ENRECV:AABBCCDDEE01,1,-38,01

AT+ENSEND=AABBCCDDEE01,2,0207
OK
+ENSENDOK:AABBCCDDEE01
```

**Board A's host then sees:**
```
+ENRECV:AABBCCDDEE02,2,-40,0207
```
Parses `02` as response type, `07` as the status value.

### 8.5 Larger payload — fragmented send

Board A pushes a 600-byte config blob to Board B (exceeds classic 250B frame
limit):

```
AT+ENFRAGSEND=AABBCCDDEE02,600,<600 bytes as 1200 hex chars>
OK
+ENFRAGRECV:AABBCCDDEE02,1,3
+ENFRAGRECV:AABBCCDDEE02,2,3
+ENFRAGRECV:AABBCCDDEE02,3,3
+ENSENDOK:AABBCCDDEE02
```

Board B's host only sees the fragment-progress URCs (useful for a progress
indicator) followed by one final reassembled receive once all fragments land:

```
+ENRECV:AABBCCDDEE01,600,-45,<600 bytes as 1200 hex chars>
```

If a fragment is lost and retry/reassembly ultimately fails, Board A gets
`+ENSENDFAIL` instead of `+ENSENDOK`, and Board B never emits the final
`+ENRECV` — host should treat "some FRAGRECV but no final RECV" as a hung
transfer and time out on its own.

---

*Implements the "AT+EN Command Set Specification v0.1 draft" — iLabs
Challenger+ platform, ESP32 slave co-processor.*

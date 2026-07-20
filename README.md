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
- One-command device discovery (`AT+ENDISCOVER`): a central unit scans the
  channel and gets the MAC (and RSSI) of every listening board — responders
  answer in firmware, no host involvement.
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
sdkconfig.defaults.esp32c6  C6 overrides (console moved off the AT pins)
sdkconfig.defaults.esp32c3  C3 overrides (console moved off the AT pins)
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
| `EN_UART_BAUD` | `115200` | AT link baud rate at boot (change at runtime with `AT+ENBAUD`) |
| `EN_UART_FLOWCTRL` | `EN_UART_FLOWCTRL_NONE` | `NONE`, `RTS`, `CTS` or `CTS_RTS` |
| `EN_UART_RTS_THRESH` | `100` | RX FIFO level that de-asserts RTS |
| `EN_UART_TX_PIN` / `EN_UART_RX_PIN` | C6: 16/17, C3: 21/20 | AT UART data pins |
| `EN_UART_RTS_PIN` / `EN_UART_CTS_PIN` | C6: 19/18, C3: `EN_PIN_NC` | flow control pins (`EN_PIN_NC` if unused) |
| `EN_FRAG_MAX_TOTAL` | `4096` | max `AT+ENFRAGSEND` payload / reassembly buffer |
| `EN_FRAG_RX_SLOTS` | `4` | concurrent fragmented-receive sources |
| `EN_SEND_CB_TIMEOUT_MS` | `1000` | max wait for the ESP-NOW send callback |
| `EN_PING_TIMEOUT_MS` | `500` | `AT+ENPEERCHECK` round-trip timeout |
| `EN_FRAG_TIMEOUT_MS` | `3000` | reassembly timeout (stale transfers dropped) |
| `EN_RAW_DATA_TIMEOUT_MS` | `10000` | `AT+ENSENDRAW` wait for raw bytes |
| `EN_DISCOVER_TIMEOUT_MS` | `1000` | default `AT+ENDISCOVER` collection window |
| `EN_DISCOVERY_RESPOND` | `1` | boot default for answering scans (runtime: `AT+ENDISCOVERABLE`) |
| `EN_DISCOVER_MAX` | `32` | max unique devices reported per scan |
| `EN_DISCOVER_JITTER_MS` | `50` | random response delay to avoid fleet collisions |
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
>
> On both the ESP32-C6 (AT UART GPIO16/17) and the ESP32-C3 (AT UART
> GPIO20/21) the AT pins are *also* the chip's default UART0 console pins, so
> the console log would collide with the AT stream. The per-target defaults
> relocate the console off those pins: `sdkconfig.defaults.esp32c6` moves it to
> GPIO2/4, and `sdkconfig.defaults.esp32c3` to GPIO18/19
> (`CONFIG_ESP_CONSOLE_UART_TX_GPIO` / `_RX_GPIO`). The mask-ROM's first-stage
> boot log still prints briefly on the default pins at reset; the host should
> discard AT input until `+ENREADY`.

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
ESP-IDF. It builds against **[ESP8266_RTOS_SDK v3.4](https://github.com/espressif/ESP8266_RTOS_SDK)**
and its own xtensa-lx106 toolchain — a separate install from ESP-IDF.

#### One-time toolchain setup

```sh
mkdir -p ~/esp && cd ~/esp
git clone -b v3.4 --recursive https://github.com/espressif/ESP8266_RTOS_SDK.git
cd ESP8266_RTOS_SDK
./install.sh          # xtensa-lx106 toolchain + Python env (into ~/.espressif)
```

ESP8266_RTOS_SDK v3.4 dates from 2020, so on a current Linux host — **CMake ≥ 4,
Python ≥ 3.12, PEP-668 "externally-managed" system Python** — the stock
install/build trips over a few version gaps. Each has a one-line fix (skip any
that don't apply to your host):

**`install.sh` aborts: "Can not perform a '--user' install … not visible in
this virtualenv".** The installer bootstraps `virtualenv` with `pip install
--user`, which pip refuses inside a virtualenv or an externally-managed Python.
Run the installer from a throwaway venv that already has `virtualenv`, so it
skips that bootstrap:

```sh
python3 -m venv ~/esp/py-bootstrap
. ~/esp/py-bootstrap/bin/activate
pip install --upgrade pip virtualenv
./install.sh          # from inside ~/esp/ESP8266_RTOS_SDK
deactivate
```

**`export.sh`/build fails: "pkg_resources cannot be imported".** The SDK's
dependency check imports `pkg_resources`, which setuptools ≥ 81 removed. Pin the
SDK's own Python env back below that (match the env name `install.sh` created):

```sh
~/.espressif/python_env/rtos3.4_py3.12_env/bin/python -m pip install "setuptools<81"
```

**CMake errors: "Compatibility with CMake < 3.5 has been removed".** The old SDK
declares an ancient `cmake_minimum_required`; tell CMake 4.x to tolerate it with
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` (already in the build command below).

> The SDK's pthread component also mis-names the force-include anchor for its
> condition-variable implementation, which otherwise breaks the final C++ link
> (`undefined reference to pthread_cond_init`). That one is already worked around
> in `main/CMakeLists.txt` for the `esp8266` target — no action needed.

#### Build & flash

The ESP8285 defaults file sets 2 MB flash, DOUT mode (mandatory for the on-die
flash) and moves the console to UART1, so the AT link keeps UART0:

```sh
export IDF_PATH=~/esp/ESP8266_RTOS_SDK      # adjust
. $IDF_PATH/export.sh

rm -rf build sdkconfig                       # wipe any ESP-IDF (C3/C6) build tree
idf.py -DSDKCONFIG_DEFAULTS=sdkconfig.defaults.esp8285 \
       -DCMAKE_POLICY_VERSION_MINIMUM=3.5 build
idf.py -p /dev/ttyUSB0 flash monitor
```

The two SDKs (ESP-IDF for C3/C6, ESP8266_RTOS_SDK for the 8285) **cannot share a
`build/` directory**, so `rm -rf build` whenever you switch between them. The
8266 SDK also accepts only a single defaults file, which is why
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
| `AT+CGMI` | `AT+CGMI` | manufacturer identification (`iLabs Electronics`) |
| `AT+CGMM` | `AT+CGMM` | model identification (`ESP32-C6 ESP-NOW`, per target) |
| `AT+CGMR` | `AT+CGMR` | firmware revision (`<fw_ver>`) |
| `AT+ENCHANNEL` | `AT+ENCHANNEL?` / `=<ch>` | get/set WiFi channel |
| `AT+ENRATE` | `AT+ENRATE=<rate_idx>` | set PHY rate (see below) |
| `AT+ENBAUD` | `AT+ENBAUD?` / `=<baud>` | get/set AT link baud rate (standard rates ≤ 921600) |
| `AT+ENFLOW` | `AT+ENFLOW?` / `=<mode>` | get/set AT link HW flow control: `0` none, `1` RTS, `2` CTS, `3` RTS+CTS |
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
| `AT+ENDISCOVER` | `AT+ENDISCOVER` / `=<timeout_ms>` | scan for devices: one `+ENDISCOVER:<mac>,<rssi>` line per responder |
| `AT+ENDISCOVERABLE` | `AT+ENDISCOVERABLE?` / `=<0\|1>` | opt this device out of / into answering scans |

### Identity commands (`AT+CGMI` / `AT+CGMM` / `AT+CGMR`)

These follow the cellular-modem convention (3GPP TS 27.007), so a host that
already speaks to LTE modems can probe this device the same way. Each is an
execution command (no `?`/`=`) that answers with a bare identity string
followed by `OK`:

```
AT+CGMI
iLabs Electronics
OK

AT+CGMM
ESP32-C6 ESP-NOW
OK

AT+CGMR
1.0.0
OK
```

`AT+CGMR` reports the firmware version — the same string as the first field
of `AT+ENVER?`. The `AT+CGMM` model tracks the build target (`ESP32-C3
ESP-NOW`, `ESP8285 ESP-NOW`, …); both strings live in `en_at_config.h`.

### URCs (unsolicited result codes)

| URC | Meaning |
|---|---|
| `+ENREADY` | firmware booted / reset |
| `+ENRECV:<src_mac>,<len>,<rssi>,<payload_hex>[,<dst_mac>]` | inbound frame (mandatory always-on); optional `<dst_mac>` distinguishes broadcast from unicast |
| `+ENSENDOK:<mac>` / `+ENSENDFAIL:<mac>` | delivery result after `OK` of a send |
| `+ENFRAGRECV:<mac>,<frag_idx>,<frag_total>` | fragment progress (both directions, 1-based) |

> **`+ENRECV` destination field:** on ESP-IDF targets (C3/C6) `+ENRECV`
> appends a fifth field, the frame's destination MAC, so the host can tell a
> broadcast (`FFFFFFFFFFFF`) from a unicast (this device's own MAC). The field
> is appended after the payload, so existing 4-field parsers are unaffected;
> it is omitted on the ESP8285, whose receive callback carries no destination.

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

### Device discovery (`AT+ENDISCOVER`)

`AT+ENDISCOVER` (default window `EN_DISCOVER_TIMEOUT_MS`, 1000 ms) or
`AT+ENDISCOVER=<timeout_ms>` (50–30000) broadcasts a discovery probe and
collects answers for the given window, then reports every unique responder:

```
AT+ENDISCOVER=1000
+ENDISCOVER:AABBCCDDEE02,-42
+ENDISCOVER:AABBCCDDEE03,-55
+ENDISCOVER:AABBCCDDEE07,-71
OK
```

- Any board running this firmware answers **automatically in firmware** —
  the responder's host MCU is not involved and sees nothing.
- A device can opt out of scans: `AT+ENDISCOVERABLE=0` stops it answering
  probes (`=1` re-enables, `?` queries). The boot default is the
  `EN_DISCOVERY_RESPOND` build option, and a reset restores it — set the
  default to `0` and have the host issue `AT+ENDISCOVERABLE=1` only during
  a commissioning window (e.g. after a pairing-button press) for the
  classic "discoverable for 60 seconds" pattern. Note this only silences
  the discovery reply; the device's ordinary traffic remains visible to
  anyone sniffing the channel.
- Responders wait a random 0–`EN_DISCOVER_JITTER_MS` (50 ms default) before
  answering so a large fleet doesn't collide; up to `EN_DISCOVER_MAX` (32)
  unique devices are reported per scan. The RSSI is measured by the
  scanning device, so it reads `0` when the central is an ESP8285 (no RSSI
  there) — responder chips don't matter.
- Answering a probe momentarily adds the prober as a transient peer entry
  on the responder (removed right after the reply) — `AT+ENLISTPEER?` is
  unaffected.
- Discovery only covers the **current channel**; to sweep, step
  `AT+ENCHANNEL=<n>` through 1–13 and scan each.
- **Security note:** probes and responses are unauthenticated broadcast
  traffic — anyone on the channel can enumerate devices. Treat discovery
  as a commissioning/diagnostic tool; pair with PMK/LMK for real traffic.

**Interop with native ESP-IDF nodes:** devices not running this interpreter
ignore probes (they see them as an unknown 3-byte broadcast payload) and
therefore don't appear in scans. To make a native node discoverable,
implement this in its receive callback:

1. On receiving broadcast payload `EA 05 <token>`: add the sender as an
   unencrypted peer (if unknown), wait a random 0–50 ms, and unicast
   `EA 06 <token>` back to it.
2. That's all — the token is echoed verbatim, no state is kept.

### Changing the link baud rate (`AT+ENBAUD`)

`AT+ENBAUD=<baud>` accepts only the standard rates
`1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800,
921600`; anything else returns `ERROR` with no change. The `OK` is
transmitted **at the old rate** (the TX FIFO is drained first), then the
UART switches — so the host sequence is:

```
AT+ENBAUD=921600
OK                  <- still received at the old rate
                    <- host now reconfigures its own UART to 921600
AT
OK                  <- confirms the new rate works
```

The setting is runtime-only: after a reset the link comes back up at
`EN_UART_BAUD` (115200 by default) and emits `+ENREADY` there, so a host
using a faster rate should fall back to the default rate whenever it sees
the link go quiet after a slave reset.

### Hardware flow control (`AT+ENFLOW`)

`AT+ENFLOW=<mode>` turns UART RTS/CTS flow control on or off at runtime,
`AT+ENFLOW?` reports the current mode:

| mode | meaning |
|---|---|
| `0` | none (3-wire TX/RX/GND) |
| `1` | RTS only — the slave pauses the host when its RX FIFO fills |
| `2` | CTS only — the slave pauses its TX when the host de-asserts CTS |
| `3` | RTS+CTS — full bidirectional flow control |

This matters for **high-throughput bulk transfers**: when the host
(RP2040/RP2350) is busy and can't drain the UART in time, RTS lets the
slave hold off instead of overrunning the host; CTS lets the host throttle
the slave the same way. Enable it (`3`) before streaming large
`AT+ENSENDRAW` / fragmented payloads, and wire RTS/CTS as well as TX/RX.

On ESP32-C3/C6 the RTS/CTS pins (`EN_UART_RTS_PIN` / `EN_UART_CTS_PIN`,
routed through the GPIO matrix) are assigned on demand, so flow control can
be switched on even if the build defaulted to `EN_UART_FLOWCTRL_NONE`. Like
`AT+ENBAUD`, the `OK` is transmitted **at the old setting** (TX drained
first) and the change takes effect immediately after — so the host should
enable its own flow control only after it sees the `OK`:

```
AT+ENFLOW=3
OK                  <- still sent without flow control
                    <- host now enables RTS/CTS on its own UART
AT
OK                  <- confirms the link works with flow control
```

The setting is runtime-only and resets to `EN_UART_FLOWCTRL` (build
default) after a slave reset. On the ESP8285 flow control is pin-fixed
(RTS=GPIO15, CTS=GPIO13) and unavailable when `EN_UART_SWAP_IO` is set —
`AT+ENFLOW=<non-zero>` then returns `ERROR`.

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
(`0xEA` magic + frame type) so that data, fragments, the liveness probe and
discovery frames can be told apart on the receive side. Consequences:

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

### 8.6 Central unit scanning for available devices (firmware extension)

A gateway/central board discovers every listening board on the channel and
pairs with the ones it wants — no MACs exchanged out-of-band. The sensor
boards' hosts do nothing: their slaves answer the scan in firmware.

**Central:**
```
AT+ENINIT=6
OK

AT+ENDISCOVER=1000
+ENDISCOVER:AABBCCDDEE02,-42
+ENDISCOVER:AABBCCDDEE03,-55
OK

AT+ENADDPEER=AABBCCDDEE02,6,0
OK
AT+ENPEERCHECK=AABBCCDDEE02
+ENPEERCHECK:AABBCCDDEE02,5
OK
```

The central's host now holds the device list (sorted by RSSI if useful) and
can proceed with normal pairing — including switching to encrypted peers via
`AT+ENPMK`/`AT+ENADDPEER=...,1` once it has decided which devices belong to
it. Since discovery is unauthenticated, a production flow should verify
discovered devices at the application layer (e.g. a signed hello exchanged
over `AT+ENSEND`) before trusting them.

## Testing / spec-conformance

This firmware is exercised end-to-end by the **RegressionSuite** sketch in the
companion host library
[`iLabs_ESP-NOW`](https://github.com/PontusO/iLabs_ESP-NOW)
(`examples/RegressionSuite`, with rig notes in `extras/REGRESSION.md`). It is a
two-board self-test: flash the same sketch to two Challenger boards, and the one
with the lower MAC becomes the TESTER, drives the scored sequence, and prints a
PASS/FAIL report. Run it after changing this firmware to catch drift away from
the AT+EN Command Set Spec v0.1.

The suite runs in two phases:

- **Phase 1 — raw AT protocol.** Sends AT lines straight to the interpreter and
  asserts the *exact* terminal result, so this is the real conformance
  tripwire. It covers both the OK/query paths **and negative paths**: malformed
  input must be rejected with the precise response documented above, not
  silently accepted. What it pins:
  - **Grammar/dispatch** — unknown command → `+ENERR:8`; a non-`AT+` line, a
    trailing char after `?`, a SET on a query-only command (`AT+ENVER=1`), and a
    QUERY on an exec-only command (`AT+CGMI?`) → plain `ERROR`.
  - **Range guards** — out-of-range channel (`AT+ENINIT=0/15`, `AT+ENCHANNEL=15`),
    `AT+ENDISCOVER` window outside 50–30000, `AT+ENFLOW` mode > 3, and an invalid
    `AT+ENBAUD` value → plain `ERROR`.
  - **MAC / key validation** — a malformed or wrong-length MAC and a too-few-args
    `AT+ENADDPEER` → plain `ERROR`; a bad-length/`non-hex` PMK/LMK and a
    bad-length peer LMK → `+ENERR:6`; `encrypt` field > 1 → plain `ERROR`.
  - **Data-path length/hex** — `AT+ENSEND`/`AT+ENBCAST` payload of 0 or > 248 B
    and `AT+ENFRAGSEND` > 4096 B → `+ENERR:4`; a hex string whose length ≠ 2×len,
    or any non-hex payload → plain `ERROR`.
  - **Target-specific behavior** — the co-processor family is detected at runtime
    from `AT+CGMM`, and the `AT+ENRATE` / `AT+ENRSSI` assertions branch on it
    (e.g. the ESP8285 build returns `+ENERR:8` for any rate command and never
    reports RSSI, whereas the C6/C3 build rejects an out-of-range rate index with
    plain `ERROR`). See [ESP8285 target notes](#esp8285-target-notes).
- **Phase 2 — host library API.** Exercises the C++ surface and the OTA
  round-trips (unicast single-frame + fragmented, broadcast, and an encrypted
  unicast round-trip), plus library-level negative/boundary cases.

Because the negative assertions check the specific `+ENERR:<n>` code vs plain
`ERROR` for each rejection, a change that (for example) starts accepting an
over-length payload, drops an init/range check, or returns the wrong error code
shows up as a named FAIL that points straight at the regressed command.

---

*Implements the "AT+EN Command Set Specification v0.1 draft" — iLabs
Challenger+ platform, ESP32 slave co-processor.*

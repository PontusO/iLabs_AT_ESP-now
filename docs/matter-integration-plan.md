# Plan — AT co-processor: ESP-NOW + Matter (C6), designed for an effortless merge

Status: **in progress**. This plan builds two firmwares now — the existing ESP-NOW
interpreter and a new **C6-only Matter** interpreter — while sharing enough design that
unifying them into one binary later is a small, mechanical step.

Progress: **Phase A complete (A1–A4), v1.1.0 line.** `components/at_core` extracted (UART
transport + subsystem-agnostic parser engine), the static `s_cmds[]` dispatch replaced with
the `at_register_commands()` API (C1), the config split done (C4) with the shared transport
macros de-branded `EN_UART_*`/`EN_TARGET_ESP8266` → `AT_UART_*`/`AT_TARGET_ESP8266`, and the
radio/link owner `link_mgr` introduced (C3): `en_core` no longer calls
`esp_wifi_*`/`esp_netif_*` for lifecycle/channel, only its own ESP-NOW PHY-rate tuning.
Behaviour byte-identical throughout: builds green on C6/C3/8285, RegressionSuite 99/99 on
hardware. `at_core` is now a clean, ESP-NOW-agnostic base ready for the Matter app (Phase B).

## 0. Goal & scope

- **Two images now, one design.** Ship ESP-NOW and Matter as separate single-purpose
  firmwares; the host reflashes the C6 over UART to switch role (host-side OTA, no dual
  slot needed). Both speak the same AT conventions over the same UART.
- **Matter = C6-only, over-WiFi, mode-switched.** No simultaneous ESP-NOW + Matter for v1
  (coexistence is a later, optional nicety). WiFi transport (not Thread): reuses the WiFi
  stack, needs no Thread Border Router, and channel-coupling is moot when the two never run
  together.
- **Merge is a first-class requirement.** Every shared boundary below exists so a future
  unified build is "register both command tables + one radio owner," not a rewrite.

## 1. Target architecture (end state)

```
                 ┌────────────────────────────────────────┐
                 │            components/at_core            │  ← shared, both images
                 │  at_uart (transport)                     │
                 │  parser engine (line asm, args, hex/mac, │
                 │     AT/ATE, OK/ERROR/+xxERR mapping)     │
                 │  at_register_commands(table,count)  API  │
                 │  link_mgr: bring_up(mode)  abstraction   │
                 └───────────────▲───────────────▲──────────┘
                                 │               │
              ┌──────────────────┴───┐   ┌───────┴──────────────────┐
              │  app: espnow (C6/C3/ │   │  app: matter (C6 only)    │
              │  8285)               │   │                           │
              │  registers AT+EN…    │   │  registers AT+MT…         │
              │  en_core (ESP-NOW)   │   │  esp_matter runtime model │
              │  link_mgr: ESPNOW    │   │  link_mgr: MATTER (netif) │
              └──────────────────────┘   └───────────────────────────┘
                                 (future) app: unified — registers BOTH tables
```

## 2. Design invariants (the contracts both firmwares must honor)

These are the whole point of the exercise. If all six hold, the merge is mechanical.

- **C1 — AT-core registration API.** Dispatch is driven by `at_register_commands(const
  at_command_t *table, size_t n)`, not a single hardcoded `s_cmds[]`. Each subsystem owns
  and registers its own table at init. Merged build = both register; core is unchanged.
- **C2 — Disjoint namespaces + partitioned result codes.** `AT+EN…` vs `AT+MT…`, reserved
  now. Error codes get non-overlapping ranges (e.g. `+ENERR:1–99`, `+MTERR:100–199`) or a
  unified `+ERR` space. Reserve today; renumbering later is painful.
- **C3 — Link/radio manager.** No subsystem calls `esp_wifi_*`/`esp_netif_*` inline.
  Everything goes through `link_mgr_bring_up(LINK_MODE_ESPNOW | LINK_MODE_MATTER)` and
  `link_mgr_tear_down()`. ESP-NOW uses the minimal (no-netif) path; Matter uses the full
  netif+IP path. A merged binary then has exactly one radio owner both subsystems ask.
  **This is the contract that is expensive to retrofit — get it right first.**
- **C4 — Config conventions.** `en_at_config.h` splits into `at_core` config (UART port,
  pins, baud, buffers, flow control) shared by both, plus subsystem config layered on top.
- **C5 — URC/response conventions.** Same framing everywhere: terminal `OK`/`ERROR`,
  `+xxERR:<n>`, async URCs, and a shared `+ENREADY`-style boot marker, so the host protocol
  looks identical whichever personality is flashed.
- **C6 — Repo/build layout.** One repo, one shared `components/at_core`, two app projects
  (recommended below). Submodule/second-repo layouts add friction against "effortless."

## 3. Phased plan

### Phase 0 — De-risk by measuring (parallel with Phase A; ~0.5 day)
Independent scratch work, touches nothing in this repo.
1. Build the stock `esp-matter` on/off-light for **C6** via the managed component
   (`idf.py add-dependency "espressif/esp_matter^1.4.0"`), IDF v5.5.x.
2. Record **real** flash size, RAM/free-heap (boot and post-commissioning), and the
   partition layout. Confirm it fits 4 MB with the headroom we expect (~1.9 MB app slot).
3. Confirm the **IDF version** esp-matter wants (docs say v5.5.4) vs what this firmware
   currently builds against (5.5.x) — decide whether an IDF bump is needed.
4. Note the **mDNS** situation (ESP-IDF mdns vs CHIP Minimal mDNS) — only a concern for a
   future merged/discovery build, but record it.
   **Exit:** hard numbers + partition + IDF decision; go/no-go confidence on 4 MB.

### Phase A — Extract `at_core` from the ESP-NOW firmware (standalone value; regression-green)
Pure refactor of the existing firmware; **no behavior change**, verified against the
99-check RegressionSuite after each step (Phase 1 of that suite exercises the parser
directly, so any drift shows up immediately).
- **A1 — Create `components/at_core`.** Move `at_uart.c/.h`. Split `at_parser.c` into the
  reusable **engine** (line assembly, `split_args`, hex/MAC/uint parsing, `AT`/`ATE`
  handling, result→`OK`/`ERROR`/`+xxERR` mapping, the dispatch loop) and leave the
  ESP-NOW **command handlers** in the app.
- **A2 — Registration API (C1).** Replace the static `s_cmds[]` dispatch with
  `at_register_commands()`; ESP-NOW registers its table at init. Regression: 99/99.
- **A3 — Link/radio manager (C3).** Introduce `link_mgr` with an explicit `bring_up(mode)`;
  route today's ESP-NOW WiFi bring-up through the `LINK_MODE_ESPNOW` path. No functional
  change, but the seam now exists.
- **A4 — Config split (C4).** Factor `en_at_config.h` into `at_core` transport config +
  ESP-NOW config. Keep all current values (C6 16/17, C3 21/20, 8285 fixed 1/3, etc.).
- **Verify:** build C6 + C3 + 8285; RegressionSuite 99/99 on real hardware; diff URC/OK/
  ERROR output against a saved baseline.

### Phase B — Matter firmware (C6-only)
- **B1 — App skeleton on `at_core`.** New `matter` app that links `components/at_core` and
  registers a tiny table (`AT`, identity, `AT+MT?` version). Proves the core is reusable in
  a second image before any Matter code. C6-only target guard.
- **B2 — Bring up esp-matter.** Add the managed component; hardcode a single on/off
  endpoint; Matter-over-WiFi; commission with `chip-tool`; toggle the attribute. Acceptance:
  commissions and controls from a real Matter controller.
- **B3 — Matter mode through `link_mgr` (C3).** Full WiFi+netif via the shared abstraction's
  `LINK_MODE_MATTER` path (mirror of A3).
- **B4 — `AT+MT` command set v1** (maps 1:1 onto the esp_matter runtime API the research
  confirmed):
  - Lifecycle: `AT+MTINIT`, `AT+MTRESET` (factory reset), `AT+MTSTATE?`.
  - Commissioning: `AT+MTCOMMISSION` → `OpenBasicCommissioningWindow`; URCs
    `+MTCOMMISSION:STARTED|COMPLETE|FAILED` from the platform `event_callback`;
    `AT+MTCODES?` (onboarding/QR/manual code); `AT+MTFABRICS?`.
  - Data model: `AT+MTEP=<devtype>` (`endpoint::create` + `add_device_type` + `enable`),
    `AT+MTATTR=<ep>,<cluster>,<attr>,<val>` (`attribute::update`), `AT+MTATTR?…`
    (`attribute::get_val`); URC `+MTATTR:<ep>,<cluster>,<attr>,<val>` on controller-driven
    writes (from the attribute update callback) so the host sees external changes.
  - State/diag: `AT+MTSTATE?` (uninit/commissioning/operational), fabric count.
- **B5 — Host flashing / mode-switch.** Document the reflash-to-switch flow; add a Matter
  image to the host flashing bundle **later** (that bundle work is out of scope here and on
  hold per current instruction).
- **Verify:** commission + attribute read/write/report via `chip-tool`; later, add an
  `AT+MT` phase to the RegressionSuite mirroring the ESP-NOW negative/positive coverage.

### Phase C — Merge readiness (carried throughout; validated at the end)
- Maintain a **namespace + error-code registry** (one short doc/table) as commands are added.
- Write a short **"how to merge" appendix**: a `unified` app that calls both
  `en_register()` and `mt_register()`, with `link_mgr` arbitrating the radio and a guard
  that only one mode is active (until/unless coexistence is pursued).
- **Merge smoke test:** a throwaway unified build that registers both tables and answers
  both `AT+EN…` and `AT+MT…` (mode-switched) — proves the contracts held. Not shipped.

## 4. Key decisions & rationale
- **Managed component, not full clone** — avoids the large connectedhomeip submodule tree
  and heavy bootstrap; lighter CI/build.
- **Matter-over-WiFi, not Thread** — reuses the WiFi stack, no Border Router, and coexistence
  (the only reason to prefer Thread) is out of scope for v1.
- **Mode-switch via host reflash** — the host already has the tooling (USB2Serial + esptool);
  removes all partition/dual-image pressure and keeps each image single-purpose.
- **Refactor ESP-NOW first** — the `at_core` extraction has standalone value and is fully
  guarded by the regression suite, so the shared foundation lands with zero Matter risk.

## 5. Risks & open questions
- **IDF version alignment.** esp-matter pins ~v5.5.4; confirm the exact 5.5.x this firmware
  uses and whether a bump disturbs the C3/8285 builds. (8285 is a different SDK entirely, so
  unaffected; C6/C3 share IDF.)
- **RAM headroom.** Free heap after Matter+BLE+WiFi init was ~36 KB (boot) → ~102 KB (post-
  commissioning) in Espressif's C3 test. Watch the AT parser task stack; measure on C6.
- **DAC / factory data.** Real products need per-device Device Attestation Certs + passcode
  in `fctry`/`esp_secure_cert` via `esp-matter-mfg-tool`. Fine to use test creds for
  bring-up; decide the manufacturing story before shipping.
- **mDNS conflict** (ESP-IDF mdns vs CHIP Minimal mDNS) — only bites a merged/discovery
  build; note and defer.
- **Coexistence** — explicitly deferred; the `link_mgr` contract keeps the door open.
- **numbers are C3-measured** — C6 build-and-measure in Phase 0 is the confirmation.

## 6. Verification strategy
- **Phase A:** RegressionSuite 99/99 is the invariant — the parser engine extraction must
  keep every `OK`/`ERROR`/`+ENERR`/URC byte identical. This is our safety net.
- **Phase B:** `chip-tool` commissioning + attribute get/set/report as acceptance; then a
  new `AT+MT` RegressionSuite phase (positive + negative, mirroring the ESP-NOW style).
- **Merge:** the Phase C smoke build answering both command families.

## 7. Sequencing
```
Phase 0 (measure) ─┐  (parallel, independent)
Phase A (at_core) ─┴─► Phase B (matter) ─► Phase C (merge readiness)
       └ A1→A2→A3→A4, regression-green each step
```
B1 depends on A1–A2 (core must exist). C is validated last but its contracts (C1–C6) are
honored from A1 onward.

## 8. Repo/build layout (decision needed)
Recommended, least-disruptive single-repo shape:
```
components/at_core/            # shared engine + transport + link_mgr + shared config
main/                          # ESP-NOW app (stays the root project; C6/C3/8285)
matter/                        # new C6-only app: its own CMakeLists + main/ + sdkconfig
  ...  EXTRA_COMPONENT_DIRS = ../components
sdkconfig.defaults*            # existing per-target defaults; add C6-Matter defaults
docs/matter-integration-plan.md
```
Alternative considered and rejected for now: separate repo + `at_core` submodule (more
friction, works against the "effortless merge" goal).

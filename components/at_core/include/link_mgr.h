/*
 * SPDX-FileCopyrightText: 2026 Pontus Oldberg <pontus@ilabs.se>
 * SPDX-License-Identifier: MIT
 *
 * link_mgr.h - the single radio/link owner (design invariant C3).
 *
 * No AT subsystem brings WiFi/netif up or down inline. Everything goes
 * through link_mgr_bring_up(mode)/link_mgr_tear_down(), so a merged binary
 * has exactly one owner of "is the radio up, in which mode, on which
 * channel" that every AT personality asks. Modes are named for what they
 * do, not for who uses them: one personality needs the minimal WiFi-STA
 * path (no netif/IP), another needs the full netif+IP path.
 *
 * Subsystem-private PHY tuning that has no meaning across personalities
 * (e.g. the ESP-NOW long-range protocol bit and per-peer rate) stays in
 * the owning subsystem, applied after bring_up.
 */

#pragma once

typedef enum {
    LINK_MODE_NONE   = 0,   /* radio down                                  */
    LINK_MODE_ESPNOW = 1,   /* WiFi STA, no netif/IP (minimal path)        */
    LINK_MODE_IP     = 2,   /* WiFi STA + netif + IP (full path)           */
} link_mode_t;

/*
 * One-time platform bring-up: the default event loop and the netif
 * subsystem. Idempotent; call once at boot before any bring_up.
 */
void link_mgr_init(void);

/*
 * Bring the radio up in `mode` on `channel` (1..14). Returns 0 on
 * success. On failure the stack is left fully torn down and returns -1.
 */
int link_mgr_bring_up(link_mode_t mode, int channel);

/* Tear the radio back down. Safe to call when already down. */
void link_mgr_tear_down(void);

/* Currently active mode (LINK_MODE_NONE when down). */
link_mode_t link_mgr_mode(void);

/* Retune the primary channel while up. Returns 0 on success, -1 on failure. */
int link_mgr_set_channel(int channel);

/* Primary channel reported by the driver, or -1 when unknown/down. */
int link_mgr_get_channel(void);

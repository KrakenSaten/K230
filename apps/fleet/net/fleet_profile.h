/*
 * PocketFleet link profiles (docs/decisions/ADR-018): what a match may assume
 * of the link it runs on - how long to wait for an answer, and how much it
 * may send. The match reads a profile where it used to read constants.
 *
 * fleet_profile_mesh is LoRa over MeshCore, and its figures are exactly the
 * constants the match had before profiles existed (fleet_match.h): with it
 * the state machine behaves as it always did. fleet_profile_lan is a local
 * network: quick link waits and no airtime to ration.
 *
 * How long an INVITE or an ACCEPT waits for a person is not a property of
 * the link, and no profile changes it (FLEET_HUMAN_*, fleet_match.h).
 *
 * Pure data; no I/O.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETFLEET_PROFILE_H
#define POCKETFLEET_PROFILE_H

#include <stdint.h>

struct fleet_link_profile {
    const char *name;
    uint32_t retry_base_ms;         /* the first wait for an answer; the floor of the link's own estimate */
    uint32_t retry_cap_ms;          /* the longest wait between tries */
    uint32_t busy_retry_ms;         /* after the transport refused a frame as busy */
    /* The airtime governor. gov_burst 0: frames are not rationed.
     * gov_hour_ms 0: there is no hourly budget. */
    uint8_t gov_burst;
    uint32_t gov_refill_ms;
    uint32_t gov_hour_ms;
    /* Chat's stricter share of the same governor; each 0 where the governor
     * itself has nothing to hold. */
    uint8_t chat_token_reserve;
    uint32_t chat_hour_ms;
    uint32_t chat_share_ms;
};

extern const struct fleet_link_profile fleet_profile_mesh;
extern const struct fleet_link_profile fleet_profile_lan;

#endif

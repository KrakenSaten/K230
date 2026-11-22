/*
 * gatewayd: the authenticated Companion Gateway (docs/decisions/ADR-019,
 * accepted; docs/api/gateway.md). The numbers every module shares. Each one
 * is a limit ADR-019 "Limits (v1)" or docs/design/companion/PAIRING.md
 * names; changing one is changing those documents.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef GATEWAYD_GW_H
#define GATEWAYD_GW_H

#define GW_PORT 47231
#define GW_PROTO 1
#define GW_API_VERSION 0          /* the local gateway.* control API */

#define GW_FP_LEN 32              /* SHA-256 of the SubjectPublicKeyInfo */
#define GW_FP_HEX (GW_FP_LEN * 2)
#define GW_TOKEN_LEN 16
#define GW_NAME_MAX 32            /* bytes of UTF-8 */

#define GW_MAX_CLIENTS 8          /* paired */
#define GW_MAX_SESSIONS 2         /* established at once */
#define GW_MAX_PENDING 8          /* handshakes in progress */
#define GW_HANDSHAKES_PER_S 4

#define GW_FRAME_IN_MAX (64u * 1024u)
#define GW_FRAME_OUT_MAX (256u * 1024u)
#define GW_OUTQ_MAX (256u * 1024u)
#define GW_METHOD_MAX 64
#define GW_REQ_BURST 40           /* requests: a burst, then per second */
#define GW_REQ_PER_S 20

#define GW_HANDSHAKE_MS 10000
#define GW_HELLO_MS 10000
#define GW_IDLE_MS 90000
#define GW_UPSTREAM_CONNECT_MS 100
#define GW_UPSTREAM_CALL_MS 300

#define GW_PAIR_WINDOW_MS 120000
#define GW_PAIR_TOKEN_MS 30000    /* a candidate's time to present the token */
#define GW_PAIR_MAX_BAD 3

/* Error codes beyond pocketipc's 1 to 6 (docs/api/gateway.md §3). */
#define GW_ERR_FORBIDDEN 7
#define GW_ERR_NO_PROTO 8
#define GW_ERR_UPSTREAM 9
#define GW_ERR_RATE 10

#endif

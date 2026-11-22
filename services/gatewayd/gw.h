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

/* The mesh group (M4.1; docs/api/gateway.md §5 and §7). */
#define GW_MAX_INFLIGHT 8         /* requests of one client waiting for a service */
#define GW_MESH_PENDING 16        /* such requests of all clients together */
#define GW_MESH_CALL_MS 2000      /* a request to meshcored, which never blocks the loop */
#define GW_JOURNAL_MAX 512        /* messages, memory only (ADR-019 "Limits") */
#define GW_JOURNAL_MSG_MAX 8192   /* bytes of one message as JSON; a larger one is not kept */
#define GW_SYNC_LIMIT 100         /* messages in one mesh.sync answer */
#define GW_SYNC_BYTES (192u * 1024u) /* and their size together: under GW_FRAME_OUT_MAX */
#define GW_NODES_LIMIT 100        /* nodes in one mesh.nodes answer */
#define GW_NODES_OFFSET_MAX 100000
#define GW_NODES_CACHE_MS 5000    /* one upstream list serves the pages asked for meanwhile */
#define GW_NODE_EVENT_MS 10000    /* mesh.node: at most one per node in this time */
#define GW_ACTIVITY_BIN_MS 1000   /* mesh.activity_bin: one a second while there was activity */
#define GW_REQ_SOFT_MAX (32u * 1024u)  /* unread output beyond which no further request is read */
#define GW_READ_BATCH 16          /* requests of one client in one turn of the loop */
#define GW_EVENT_SOFT_MAX (GW_OUTQ_MAX / 2) /* unread output beyond which snapshot events are skipped */

#define GW_PAIR_WINDOW_MS 120000
#define GW_PAIR_TOKEN_MS 30000    /* a candidate's time to present the token */
#define GW_PAIR_MAX_BAD 3

/* Error codes beyond pocketipc's 1 to 6 (docs/api/gateway.md §3). */
#define GW_ERR_FORBIDDEN 7
#define GW_ERR_NO_PROTO 8
#define GW_ERR_UPSTREAM 9
#define GW_ERR_RATE 10

#endif

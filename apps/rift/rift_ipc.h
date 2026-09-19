/*
 * RIFT's meshcored client: one connection, asynchronous throughout.
 *
 * The one hard rule is that nothing here may stop the LVGL thread. A UI
 * that blocks on a service freezes the panel, and Doors has already paid
 * for that once (unit A, 2026-09-08: an app tick calling radiod without a
 * deadline froze the display until the service answered). So this client
 * does not use pocketipc_call at all:
 *
 *   - requests are written and forgotten; the reply is matched by id when
 *     it turns up, which is what pocketipc's ids are for;
 *   - the socket is read non-blocking, in bounded passes, from an LVGL
 *     timer;
 *   - the only bounded wait in the whole module is the connect, and it is
 *     shorter than one frame.
 *
 * The other reason to do it this way is event loss. pocketipc_call
 * discards events while it waits for its reply (docs/api/pocketipc.md), so
 * a client that subscribes *and* calls on the same connection drops
 * whatever arrives during a call. Matching by id keeps one connection and
 * loses nothing.
 *
 * What it talks: only what Phase 1 needs - mesh.info, mesh.status,
 * mesh.identity, mesh.nodes, mesh.node, mesh.subscribe, mesh.unsubscribe,
 * and the mesh.state, mesh.node and mesh.activity events. It sends nothing
 * that transmits: no mesh.send, no mesh.advert. Opening RIFT must not put
 * anything on the air.
 *
 * No LVGL: the connection, the reconnect and the framing are host-tested
 * against a real socket and a scripted service (tests/rift_ipc_test.c).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RIFT_IPC_H
#define RIFT_IPC_H

#include "rift_model.h"

#include "pocketipc/pocketipc.h"

#include <stdint.h>

/* The service this app reads. Named once. */
#define RIFT_SERVICE "meshcored"

/* Long enough to reach a service that is there, shorter than one 60 Hz
 * frame, and it is the only wait in the module. A service that is alive
 * but not accepting fills its backlog with abandoned connections, so the
 * bounded connect of pocketipc_connect_timeout is the one to use. */
#define RIFT_CONNECT_TIMEOUT_MS 120

/* How often the client asks again for what events do not carry: the
 * service state and counters, and a whole node list. Events keep the nodes
 * current between snapshots; the snapshot exists so a node the service
 * has forgotten stops being shown here. */
#define RIFT_STATUS_PERIOD_MS 2000
#define RIFT_NODES_PERIOD_MS 20000

/* Reconnect backoff. meshcored's own ceiling is 30 s, which is right for a
 * daemon and wrong here: somebody is looking at the screen, and a screen
 * that takes half a minute to notice the service came back reads as
 * broken. Doubling from half a second to five. */
#define RIFT_BACKOFF_MIN_MS 500
#define RIFT_BACKOFF_MAX_MS 5000

/* Frames read in one pass. A burst is drained over several passes rather
 * than in one, so no single pass can hold a frame; at the air rates this
 * radio runs at (SF8, 62.5 kHz) this is orders of magnitude of headroom. */
#define RIFT_FRAMES_PER_POLL 128

/* Requests outstanding at once. Five go out on every connect and one more
 * when a node is selected; sixteen is room to spare. */
#define RIFT_MAX_PENDING 16

enum rift_req {
    RIFT_REQ_NONE = 0,
    RIFT_REQ_SUBSCRIBE,
    RIFT_REQ_UNSUBSCRIBE,
    RIFT_REQ_INFO,
    RIFT_REQ_STATUS,
    RIFT_REQ_IDENTITY,
    RIFT_REQ_NODES,
    RIFT_REQ_NODE,
};

struct rift_pending {
    int id;
    enum rift_req what;
};

struct rift_ipc {
    struct rift_model *model;
    char service[32];

    int fd;                       /* -1 when down */
    struct pocketipc_reader reader;
    int subscribed;

    int next_id;
    struct rift_pending pending[RIFT_MAX_PENDING];

    int64_t next_attempt_ms;
    int backoff_ms;
    int64_t last_status_ms;
    int64_t last_nodes_ms;

    /* Counters a screen may show, and a test may check. */
    unsigned connects;
    unsigned disconnects;
    unsigned frames_in;
    unsigned events_in;
    unsigned replies_in;
    unsigned errors_in;           /* the service answered with an error */
    unsigned bad_frames;          /* invalid JSON, or a frame not for us */
    unsigned requests_out;
    unsigned requests_refused;    /* the pending table was full */

    /* Bumped whenever something a screen draws has changed, so the app
     * repaints because something happened rather than on a timer. */
    unsigned revision;

    char last_error[96];
};

void rift_ipc_init(struct rift_ipc *c, struct rift_model *m, const char *service);

/* One pass: connect if it is time, read what is waiting, ask again for
 * what has aged out. now_ms is CLOCK_MONOTONIC (rift_mono_ms). */
void rift_ipc_poll(struct rift_ipc *c, int64_t now_ms);

/* Ask for one node by public key or prefix (mesh.node). Returns 0 when the
 * request went out, -1 when there is no connection or no room for it. */
int rift_ipc_request_node(struct rift_ipc *c, const char *key);

/* Ask for a fresh node list now, rather than at the next period. */
int rift_ipc_request_nodes(struct rift_ipc *c);

int rift_ipc_connected(const struct rift_ipc *c);

/* Unsubscribe, close and forget. Safe on a client that is already down and
 * safe to call twice, because destroy and a failed create both reach it. */
void rift_ipc_close(struct rift_ipc *c);

#endif

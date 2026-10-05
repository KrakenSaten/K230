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
 * What it talks: mesh.info, mesh.status, mesh.identity, mesh.nodes,
 * mesh.node, mesh.channels, mesh.messages, mesh.send, mesh.subscribe and
 * mesh.unsubscribe, and the mesh.state, mesh.node, mesh.channel,
 * mesh.activity and mesh.message events.
 *
 * And, on a reader's explicit request: mesh.advert, mesh.node_remove and
 * mesh.node_reset_path.
 *
 * And, from ACTIVITY's management panels (ui/rift_manage.c), each only on a
 * reader's press and none of them transmitting: mesh.channel_add (a key the
 * reader typed, pasted, or had made here - rift_keys.h - written once into
 * the request and kept nowhere), mesh.channel_remove (only after a
 * confirmation), mesh.set_name and mesh.set_path_hash; and mesh.path_hash
 * to read the size, asked on connecting.
 *
 * Two of those transmit, and each from exactly one function. mesh.send is
 * written only by rift_ipc_send_message, which is reached only from the
 * composer, by a reader pressing SEND on text a reader typed. mesh.advert is
 * written only by rift_ipc_send_advert, which is reached only from the two
 * ADVERT buttons on ACTIVITY. Nothing that happens on its own - opening the
 * app, a snapshot, a period expiring, a reconnect - can reach either, so
 * opening RIFT still puts nothing on the air. tests/rift_lint.sh checks each
 * link of both chains, and tests/rift_ipc_test.c proves it from the
 * service's side.
 *
 * Forgetting a node or its route transmits nothing; it changes what the
 * service holds, so it too is reached only from a button a reader pressed,
 * and forgetting a node only after the reader confirmed it.
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
/* Messages arrive as events, so this is the catch-up rather than the feed:
 * it exists so a message that happened during a gap in the subscription -
 * a reconnect, a burst this client was slow to drain - is still read. */
#define RIFT_MESSAGES_PERIOD_MS 30000
/* Channels change when somebody joins or leaves one, which raises an event,
 * so this is the catch-up rather than the feed - the same shape as the node
 * list, and slower, because a channel table changes far less often than a
 * mesh does. */
#define RIFT_CHANNELS_PERIOD_MS 60000
/* How many to ask for. mesh.messages with a limit answers the newest that
 * many, oldest first (docs/api/mesh.md), which is exactly the window this
 * app keeps: asking for more than it can hold would be asking the service
 * to serialise messages straight into the drop counter. */
#define RIFT_MESSAGES_LIMIT RIFT_MAX_MESSAGES

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
    RIFT_REQ_CHANNELS,
    RIFT_REQ_MESSAGES,
    RIFT_REQ_SEND,
    RIFT_REQ_ADVERT,
    RIFT_REQ_NODE_REMOVE,
    RIFT_REQ_NODE_RESET_PATH,
    RIFT_REQ_CHANNEL_ADD,
    RIFT_REQ_CHANNEL_REMOVE,
    RIFT_REQ_SET_NAME,
    RIFT_REQ_PATH_HASH,
    RIFT_REQ_SET_PATH_HASH,
    /* Repeater control (rift_ipc_repeater.c). */
    RIFT_REQ_DISCOVER,
    RIFT_REQ_DISCOVERED,
    RIFT_REQ_REMOTE_LOGIN,
    RIFT_REQ_REMOTE_REQUEST,
    RIFT_REQ_REMOTE_CLI,
    RIFT_REQ_REMOTE_LOGOUT,
    RIFT_REQ_REMOTE_SESSION,
};

struct rift_pending {
    int id;
    enum rift_req what;
};

struct rift_rxlog;

struct rift_ipc {
    struct rift_model *model;
    /* RX LOG's ring (rift_rxlog.h), or NULL: then the subscription does not
     * ask for the receive log and a mesh.rx is never expected. */
    struct rift_rxlog *rxlog;
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
    int64_t last_channels_ms;
    int64_t last_messages_ms;

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

/* Ask for the message history now. */
int rift_ipc_request_messages(struct rift_ipc *c);

/* Ask for the channel list now, rather than at the next period. */
int rift_ipc_request_channels(struct rift_ipc *c);

/* Send one message (mesh.send).
 *
 * This is the only call in RIFT that transmits, and it exists only from
 * phase 2: nothing on a screen reaches it except a reader pressing SEND on
 * text a reader typed. It records the submission in the model first, so a
 * thread can say a message is on its way without saying it arrived, and
 * fails without writing anything when a submission is already in flight or
 * the text is not one mesh.send will take.
 *
 * conv_key is a peer's public key or a channel's "#<slot>"; the one call
 * writes `to` or `channel` accordingly, so the "one place this app
 * transmits" rule holds for channels too.
 *
 * Returns 0 when the request went out, -1 otherwise; on -1 the model holds
 * the reason. */
int rift_ipc_send_message(struct rift_ipc *c, const char *conv_key, const char *text);

/* Advert this node (mesh.advert): zero-hop when zero_hop is set - heard in
 * direct range and repeated by nobody - flooded otherwise.
 *
 * The one call in RIFT that makes this node advert, reached only from the
 * ADVERT buttons a reader presses. It records the request in the model
 * (m->advert) before writing it; the service's answer is "accepted", and
 * that is all the model will ever say about it. Returns 0 when the request
 * went out, -1 otherwise, with the reason in the model. */
int rift_ipc_send_advert(struct rift_ipc *c, int zero_hop);

/* Ask the service to forget a node (mesh.node_remove), or only its learned
 * route (mesh.node_reset_path). key is the node's whole public key; label
 * is what it is called, kept for the screen to say what was done. Neither
 * transmits. Returns 0 when the request went out, -1 otherwise, with the
 * reason in the model (m->node_op). */
int rift_ipc_forget_node(struct rift_ipc *c, const char *key, const char *label);
int rift_ipc_reset_path(struct rift_ipc *c, const char *key, const char *label);

/* Managing this node, each recorded in the model's manage_op before it is
 * written (rift_model_action_begin) and answered or refused there. None of
 * them transmits. Each returns 0 when the request went out, -1 otherwise,
 * with the reason in the model.
 *
 * rift_ipc_channel_add writes name and key into the request and keeps
 * neither: the key's one copy is the service's (channels.v1). label is what
 * the screen calls the channel afterwards. */
int rift_ipc_channel_add(struct rift_ipc *c, const char *name, const char *key_b64);
/* Leave the channel in that slot: its key is forgotten by the service, and
 * nothing on the air gives it back. Reached only from a confirmation. */
int rift_ipc_channel_remove(struct rift_ipc *c, int slot, const char *label);
int rift_ipc_set_name(struct rift_ipc *c, const char *name);
int rift_ipc_set_path_hash(struct rift_ipc *c, int bytes);
/* Ask for the path hash size now (mesh.path_hash): a question. */
int rift_ipc_request_path_hash(struct rift_ipc *c);

/* Write one request (rift_ipc.c's own writer), for rift_ipc_manage.c and
 * nothing else: every screen goes through the named calls above. params is
 * consumed. Returns 0, or -1 with the connection already dropped when the
 * write failed. */
int rift_ipc_write(struct rift_ipc *c, enum rift_req what, cJSON *params, int64_t now_ms);

/* Repeater control (rift_ipc_repeater.c, rift_repeater.h). Each records
 * the request in the model's repeater block before writing it and returns 0
 * when it went out, -1 otherwise with the reason there.
 *
 * Four transmit, each reached only from a button a reader pressed:
 * rift_ipc_scan_repeaters (one zero-hop request; not written while a round
 * is open), rift_ipc_repeater_login, rift_ipc_repeater_ask (STATUS,
 * NEIGHBOURS or OWNER) and rift_ipc_repeater_cli (only a command the rule in
 * rift_repeater.h does not refuse). Logout transmits nothing.
 *
 * rift_ipc_repeater_login takes the field's password buffer and its size,
 * puts it in the one request by reference and WIPES it before returning,
 * sent or not. */
int rift_ipc_scan_repeaters(struct rift_ipc *c);
int rift_ipc_request_discovered(struct rift_ipc *c);
int rift_ipc_repeater_login(struct rift_ipc *c, const char *key, char *password, size_t cap);
int rift_ipc_repeater_ask(struct rift_ipc *c, const char *key, enum rift_rep_kind kind);
int rift_ipc_repeater_cli(struct rift_ipc *c, const char *key, const char *command);
/* End whichever session meshcored holds, and forget it here. */
int rift_ipc_repeater_logout(struct rift_ipc *c);
/* rift_ipc.c's dispatch, for a reply to one of the above. Returns 1 when
 * the reply was one of them. */
int rift_ipc_repeater_reply(struct rift_ipc *c, enum rift_req what, const cJSON *msg);
/* Each poll: a request the service never ended is let go, and a session
 * whose repeater left the node list is ended. */
void rift_ipc_repeater_poll(struct rift_ipc *c, int64_t now_ms);
/* Leaving RIFT: end the repeater session, if there is one. */
void rift_ipc_repeater_leave(struct rift_ipc *c);

int rift_ipc_connected(const struct rift_ipc *c);

/* Unsubscribe, close and forget. Safe on a client that is already down and
 * safe to call twice, because destroy and a failed create both reach it. */
void rift_ipc_close(struct rift_ipc *c);

#endif

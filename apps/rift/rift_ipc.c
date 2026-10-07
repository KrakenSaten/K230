/*
 * RIFT's meshcored client. See rift_ipc.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_ipc.h"

#include "rift_format.h"
#include "rift_rxlog.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *method_of(enum rift_req what)
{
    switch (what) {
    case RIFT_REQ_SUBSCRIBE:
        return "mesh.subscribe";
    case RIFT_REQ_UNSUBSCRIBE:
        return "mesh.unsubscribe";
    case RIFT_REQ_INFO:
        return "mesh.info";
    case RIFT_REQ_STATUS:
        return "mesh.status";
    case RIFT_REQ_IDENTITY:
        return "mesh.identity";
    case RIFT_REQ_NODES:
        return "mesh.nodes";
    case RIFT_REQ_NODE:
        return "mesh.node";
    case RIFT_REQ_CHANNELS:
        return "mesh.channels";
    case RIFT_REQ_MESSAGES:
        return "mesh.messages";
    case RIFT_REQ_SEND:
        return "mesh.send";
    case RIFT_REQ_ADVERT:
        return "mesh.advert";
    case RIFT_REQ_NODE_REMOVE:
        return "mesh.node_remove";
    case RIFT_REQ_NODE_RESET_PATH:
        return "mesh.node_reset_path";
    case RIFT_REQ_CHANNEL_ADD:
        return "mesh.channel_add";
    case RIFT_REQ_CHANNEL_REMOVE:
        return "mesh.channel_remove";
    case RIFT_REQ_SET_NAME:
        return "mesh.set_name";
    case RIFT_REQ_PATH_HASH:
        return "mesh.path_hash";
    case RIFT_REQ_SET_PATH_HASH:
        return "mesh.set_path_hash";
    case RIFT_REQ_DISCOVER:
        return "mesh.discover";
    case RIFT_REQ_DISCOVERED:
        return "mesh.discovered";
    case RIFT_REQ_REMOTE_LOGIN:
        return "mesh.remote_login";
    case RIFT_REQ_REMOTE_REQUEST:
        return "mesh.remote_request";
    case RIFT_REQ_REMOTE_CLI:
        return "mesh.remote_cli";
    case RIFT_REQ_REMOTE_LOGOUT:
        return "mesh.remote_logout";
    case RIFT_REQ_REMOTE_SESSION:
        return "mesh.remote_session";
    case RIFT_REQ_NONE:
    default:
        return NULL;
    }
}

void rift_ipc_init(struct rift_ipc *c, struct rift_model *m, const char *service)
{
    if (!c) {
        return;
    }
    memset(c, 0, sizeof(*c));
    c->model = m;
    snprintf(c->service, sizeof(c->service), "%s", service ? service : RIFT_SERVICE);
    c->fd = -1;
    c->next_id = 1;
    c->backoff_ms = RIFT_BACKOFF_MIN_MS;
    pocketipc_reader_init(&c->reader);
}

int rift_ipc_connected(const struct rift_ipc *c)
{
    return c && c->fd >= 0;
}

static void forget_pending(struct rift_ipc *c)
{
    memset(c->pending, 0, sizeof(c->pending));
}

/* Drop the connection. reason is what a screen may show; it is this app's
 * words about its own socket, never the service's. */
static void drop(struct rift_ipc *c, const char *reason, int64_t now_ms)
{
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
        c->disconnects++;
    }
    if (c->subscribed) {
        rift_rxlog_service_lost(c->rxlog, now_ms);
    }
    c->subscribed = 0;
    forget_pending(c);
    pocketipc_reader_free(&c->reader);
    pocketipc_reader_init(&c->reader);
    snprintf(c->last_error, sizeof(c->last_error), "%s", reason ? reason : "");
    rift_model_service_lost(c->model, reason);
    rift_rep_service_lost(&c->model->repeater);
    c->next_attempt_ms = now_ms + c->backoff_ms;
    if (c->backoff_ms < RIFT_BACKOFF_MAX_MS) {
        c->backoff_ms *= 2;
        if (c->backoff_ms > RIFT_BACKOFF_MAX_MS) {
            c->backoff_ms = RIFT_BACKOFF_MAX_MS;
        }
    }
    c->revision++;
}

static int remember(struct rift_ipc *c, int id, enum rift_req what)
{
    int i;

    for (i = 0; i < RIFT_MAX_PENDING; i++) {
        if (c->pending[i].what == RIFT_REQ_NONE) {
            c->pending[i].id = id;
            c->pending[i].what = what;
            return 0;
        }
    }
    return -1;
}

static enum rift_req take_pending(struct rift_ipc *c, int id)
{
    int i;

    for (i = 0; i < RIFT_MAX_PENDING; i++) {
        if (c->pending[i].what != RIFT_REQ_NONE && c->pending[i].id == id) {
            enum rift_req what = c->pending[i].what;

            c->pending[i].what = RIFT_REQ_NONE;
            c->pending[i].id = 0;
            return what;
        }
    }
    return RIFT_REQ_NONE;
}

/* Write one request. params is consumed. Returns 0, or -1 with the
 * connection already dropped when the write failed. */
static int request(struct rift_ipc *c, enum rift_req what, cJSON *params, int64_t now_ms)
{
    const char *method = method_of(what);
    cJSON *msg;
    int id;
    int rc;

    if (c->fd < 0 || !method) {
        cJSON_Delete(params);
        return -1;
    }
    id = c->next_id++;
    if (c->next_id <= 0) {
        c->next_id = 1;
    }
    /* An unsubscribe on the way out is fire and forget: the reply would
     * arrive after this app has stopped reading. */
    if (what != RIFT_REQ_UNSUBSCRIBE && remember(c, id, what) != 0) {
        c->requests_refused++;
        cJSON_Delete(params);
        return -1;
    }
    msg = cJSON_CreateObject();
    if (!msg) {
        cJSON_Delete(params);
        return -1;
    }
    cJSON_AddNumberToObject(msg, "id", id);
    cJSON_AddStringToObject(msg, "method", method);
    if (params) {
        cJSON_AddItemToObject(msg, "params", params);
    }
    rc = pocketipc_send(c->fd, msg);
    cJSON_Delete(msg);
    if (rc != 0) {
        /* The only write that can fail slowly is one to a service that has
         * stopped reading, and pocketipc bounds that at 200 ms per frame
         * (POCKETIPC_SEND_TIMEOUT_MS). A failure here is the service gone. */
        drop(c, "meshcored stopped reading", now_ms);
        return -1;
    }
    c->requests_out++;
    return 0;
}

int rift_ipc_write(struct rift_ipc *c, enum rift_req what, cJSON *params, int64_t now_ms)
{
    return c ? request(c, what, params, now_ms) : (cJSON_Delete(params), -1);
}

int rift_ipc_request_node(struct rift_ipc *c, const char *key)
{
    cJSON *params;

    if (!c || c->fd < 0 || !key || !key[0]) {
        return -1;
    }
    /* A channel is not a node, and mesh.node is a question about nodes.
     *
     * COMMS opens a conversation the same way whichever kind it is, and the
     * open used to ask the service about the peer unconditionally - so
     * opening a channel sent `{"node": "#0"}`, which meshcored refuses
     * because it is not a hex key. The refusal was invisible while the
     * service was healthy and wrong the moment it was not: the command line
     * shows the client's last error when the service goes away, and it would
     * have said the node prefix was malformed rather than that meshcored had
     * stopped answering. It also spent a pending slot and an error count on
     * every channel opened. Refused here, in the one place that knows what
     * this method is for, rather than at each caller - by its first
     * character, so no channel-shaped key of any form gets through. */
    if (key[0] == '#') {
        return -1;
    }
    params = cJSON_CreateObject();
    if (!params) {
        return -1;
    }
    cJSON_AddStringToObject(params, "node", key);
    return request(c, RIFT_REQ_NODE, params, rift_mono_ms());
}

int rift_ipc_request_nodes(struct rift_ipc *c)
{
    if (!c || c->fd < 0) {
        return -1;
    }
    c->last_nodes_ms = rift_mono_ms();
    return request(c, RIFT_REQ_NODES, NULL, c->last_nodes_ms);
}

int rift_ipc_request_messages(struct rift_ipc *c)
{
    cJSON *params;

    if (!c || c->fd < 0) {
        return -1;
    }
    params = cJSON_CreateObject();
    if (!params) {
        return -1;
    }
    cJSON_AddNumberToObject(params, "limit", RIFT_MESSAGES_LIMIT);
    c->last_messages_ms = rift_mono_ms();
    return request(c, RIFT_REQ_MESSAGES, params, c->last_messages_ms);
}

int rift_ipc_request_channels(struct rift_ipc *c)
{
    if (!c || c->fd < 0) {
        return -1;
    }
    c->last_channels_ms = rift_mono_ms();
    return request(c, RIFT_REQ_CHANNELS, NULL, c->last_channels_ms);
}

int rift_ipc_send_message(struct rift_ipc *c, const char *conv_key, const char *text)
{
    char why[RIFT_TEXT_MAX];
    int64_t now;

    if (!c || !c->model) {
        return -1;
    }
    /* Refused before anything is written, and the reason is the model's to
     * show: a composer that cleared itself and then said nothing would look
     * like a message that went. */
    if (rift_send_text_check(text, why, sizeof(why)) != 0) {
        rift_model_send_failed(c->model, why);
        c->revision++;
        return -1;
    }
    if (c->fd < 0) {
        rift_model_send_failed(c->model, "meshcored is not answering; nothing was sent");
        c->revision++;
        return -1;
    }
    if (rift_model_sending(c->model)) {
        rift_model_send_failed(c->model, "one message is already on its way");
        c->revision++;
        return -1;
    }
    now = rift_mono_ms();
    if (rift_model_send_begin(c->model, conv_key, text, now) != 0) {
        int slot = rift_key_is_channel(conv_key);
        int limit = rift_model_text_limit(c->model, conv_key);

        /* Two different refusals, said apart. A channel's limit is shorter
         * than a direct message's because this node's name travels inside a
         * channel payload, and a reader told only "too long" would not know
         * that their 150-character message was fine yesterday to a node. */
        if (slot >= 0 && !rift_model_key_channel(c->model, conv_key)) {
            rift_model_send_failed(c->model,
                                   c->model->channels_valid
                                       ? "that channel is not joined any more; nothing was sent"
                                       : "the channel list has not been read yet; nothing was sent");
        } else if (slot >= 0 && limit > 0 && (int)strlen(text) > limit) {
            char why_long[RIFT_TEXT_MAX];

            snprintf(why_long, sizeof(why_long),
                     "too long: %d bytes fit here, this node's name goes inside it", limit);
            rift_model_send_failed(c->model, why_long);
        } else {
            rift_model_send_failed(c->model,
                                   "that is not a node or channel this app can address");
        }
        c->revision++;
        return -1;
    }
    return rift_ipc_write_send(c, conv_key, text, 0, now);
}

/* The one place this app transmits a message: a first send, or a RESEND. */
int rift_ipc_write_send(struct rift_ipc *c, const char *conv_key, const char *text,
                        int64_t resend_id, int64_t now)
{
    cJSON *params = cJSON_CreateObject();

    if (!params) {
        rift_model_send_failed(c->model, "out of memory");
        c->revision++;
        return -1;
    }
    if (resend_id > 0) {
        /* The service still holds the message: it names the message and
         * nothing else, and the service sends its own text and timestamp
         * again with a new attempt (docs/api/mesh.md, mesh.send resend). */
        cJSON_AddNumberToObject(params, "resend", (double)resend_id);
    } else {
        int slot = rift_key_is_channel(conv_key);

        /* Addressing either kind. Exactly one of the two parameters is
         * written: mesh.send refuses both together rather than preferring
         * one (docs/api/mesh.md). */
        if (slot >= 0) {
            cJSON_AddNumberToObject(params, "channel", slot);
        } else {
            cJSON_AddStringToObject(params, "to", conv_key);
        }
        cJSON_AddStringToObject(params, "text", text);
    }
    if (request(c, RIFT_REQ_SEND, params, now) != 0) {
        /* request() has already dropped the connection if the write failed,
         * and rift_model_service_lost says what became of the submission. */
        if (rift_model_sending(c->model)) {
            rift_model_send_failed(c->model, "the request could not be written");
        }
        c->revision++;
        return -1;
    }
    c->revision++;
    return 0;
}

int rift_ipc_send_advert(struct rift_ipc *c, int zero_hop)
{
    enum rift_action kind = zero_hop ? RIFT_ACTION_ADVERT_NEAR : RIFT_ACTION_ADVERT_MESH;
    cJSON *params;
    int64_t now;

    if (!c || !c->model) {
        return -1;
    }
    now = rift_mono_ms();
    if (rift_model_action_begin(c->model, kind, NULL, NULL, now) != 0) {
        /* The one already on its way keeps its caption; this one is not
         * written and says nothing over it. */
        return -1;
    }
    if (c->fd < 0) {
        rift_model_action_failed(c->model, kind, "meshcored is not answering; nothing was sent",
                                 now);
        c->revision++;
        return -1;
    }
    params = cJSON_CreateObject();
    if (!params) {
        rift_model_action_failed(c->model, kind, "out of memory", now);
        c->revision++;
        return -1;
    }
    /* The one place this app makes the node advert. zero_hop is written
     * either way, so what was asked for is in the request rather than left
     * to a default on the other side. */
    cJSON_AddBoolToObject(params, "zero_hop", zero_hop ? 1 : 0);
    if (request(c, RIFT_REQ_ADVERT, params, now) != 0) {
        if (rift_model_action_busy(c->model, kind)) {
            rift_model_action_failed(c->model, kind, "the request could not be written", now);
        }
        c->revision++;
        return -1;
    }
    c->revision++;
    return 0;
}

/* The two requests that change what the service holds about one node. One
 * writer for both, so the rules are the same: a whole key, one request at a
 * time, recorded before it is written. */
static int node_request(struct rift_ipc *c, enum rift_req what, enum rift_action kind,
                        const char *key, const char *label)
{
    cJSON *params;
    int64_t now;

    if (!c || !c->model) {
        return -1;
    }
    now = rift_mono_ms();
    /* Recorded first, so a refusal below is filed against the node it was
     * about and the screen that asked can say so there. */
    if (rift_model_action_begin(c->model, kind, key, label, now) != 0) {
        if (!rift_model_action_busy(c->model, kind)) {
            rift_model_action_failed(c->model, kind, "that is not a node this app can name", now);
            c->revision++;
        }
        return -1;
    }
    if (c->fd < 0) {
        rift_model_action_failed(c->model, kind, "meshcored is not answering; nothing was asked",
                                 now);
        c->revision++;
        return -1;
    }
    params = cJSON_CreateObject();
    if (!params) {
        rift_model_action_failed(c->model, kind, "out of memory", now);
        c->revision++;
        return -1;
    }
    cJSON_AddStringToObject(params, "node", key);
    if (request(c, what, params, now) != 0) {
        if (rift_model_action_busy(c->model, kind)) {
            rift_model_action_failed(c->model, kind, "the request could not be written", now);
        }
        c->revision++;
        return -1;
    }
    c->revision++;
    return 0;
}

int rift_ipc_forget_node(struct rift_ipc *c, const char *key, const char *label)
{
    return node_request(c, RIFT_REQ_NODE_REMOVE, RIFT_ACTION_FORGET, key, label);
}

int rift_ipc_reset_path(struct rift_ipc *c, const char *key, const char *label)
{
    return node_request(c, RIFT_REQ_NODE_RESET_PATH, RIFT_ACTION_RESET_PATH, key, label);
}

static void connect_now(struct rift_ipc *c, int64_t now_ms)
{
    int fd = pocketipc_connect_timeout(c->service, RIFT_CONNECT_TIMEOUT_MS);

    if (fd < 0) {
        snprintf(c->last_error, sizeof(c->last_error), "%s is not answering (%s)", c->service,
                 strerror(errno));
        rift_model_service_lost(c->model, c->last_error);
        c->next_attempt_ms = now_ms + c->backoff_ms;
        if (c->backoff_ms < RIFT_BACKOFF_MAX_MS) {
            c->backoff_ms *= 2;
            if (c->backoff_ms > RIFT_BACKOFF_MAX_MS) {
                c->backoff_ms = RIFT_BACKOFF_MAX_MS;
            }
        }
        c->revision++;
        return;
    }
    c->fd = fd;
    c->connects++;
    c->last_error[0] = '\0';
    c->backoff_ms = RIFT_BACKOFF_MIN_MS;
    pocketipc_reader_free(&c->reader);
    pocketipc_reader_init(&c->reader);
    forget_pending(c);
    rift_model_service_found(c->model);

    /* Subscribe first, so nothing that happens while the snapshot is being
     * answered is missed: the node list and the events that change it then
     * both come from the same connection, in order. */
    if (request(c, RIFT_REQ_SUBSCRIBE, rift_rxlog_subscribe_params(c->rxlog), now_ms) != 0) {
        return;
    }
    c->subscribed = 1;
    if (request(c, RIFT_REQ_INFO, NULL, now_ms) != 0) {
        return;
    }
    if (request(c, RIFT_REQ_IDENTITY, NULL, now_ms) != 0) {
        return;
    }
    c->last_status_ms = now_ms;
    if (request(c, RIFT_REQ_STATUS, NULL, now_ms) != 0) {
        return;
    }
    c->last_nodes_ms = now_ms;
    if (request(c, RIFT_REQ_NODES, NULL, now_ms) != 0) {
        return;
    }
    c->last_channels_ms = now_ms;
    if (request(c, RIFT_REQ_CHANNELS, NULL, now_ms) != 0) {
        return;
    }
    c->last_messages_ms = now_ms;
    if (rift_ipc_request_messages(c) != 0) {
        return;
    }
    /* Last, and a service too old to know it answers "unknown method",
     * which the model takes as "no such setting here". */
    if (request(c, RIFT_REQ_PATH_HASH, NULL, now_ms) != 0) {
        return;
    }
    /* Repeater control: a session left in the service by an earlier RIFT is
     * one this app cannot show, so it is ended (a logout transmits nothing);
     * and the repeaters it discovered this run are read, as earlier ones. */
    rift_rep_service_lost(&c->model->repeater);
    if (request(c, RIFT_REQ_REMOTE_LOGOUT, NULL, now_ms) != 0 ||
        request(c, RIFT_REQ_DISCOVERED, NULL, now_ms) != 0) {
        return;
    }
    c->revision++;
}

/* One message off the wire. Returns 0, or -1 when the connection must go. */
static int dispatch(struct rift_ipc *c, cJSON *msg)
{
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(msg, "event");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    const cJSON *result;
    const cJSON *error;
    enum rift_req what;

    if (cJSON_IsString(event) && event->valuestring) {
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(msg, "data");

        c->events_in++;
        /* Repeater control's two events first (rift_repeater.h). */
        if (rift_rep_apply_event(&c->model->repeater, event->valuestring, data,
                                 rift_mono_ms())) {
            c->revision++;
            return 0;
        }
        /* The receive log's, never the model's: a mesh.rx is a reception,
         * and the model counts an event it does not know as malformed. */
        if (strcmp(event->valuestring, "mesh.rx") == 0) {
            if (c->rxlog) {
                rift_rxlog_apply(c->rxlog, data, rift_mono_ms(), rift_rxlog_wall_now());
                c->revision++;
            }
            return 0;
        }
        /* A malformed event is refused by the model and counted there. It
         * is not a reason to drop a connection: one bad event costs one
         * event, and a peer on the air must not be able to disconnect this
         * app from its own service. */
        if (rift_model_apply_event(c->model, event->valuestring, data) == 0) {
            c->revision++;
        }
        return 0;
    }
    if (!cJSON_IsNumber(id)) {
        /* Neither an event nor a response: not something this protocol
         * produces. Counted, ignored, connection kept. */
        c->bad_frames++;
        return 0;
    }
    c->replies_in++;
    what = take_pending(c, (int)id->valuedouble);
    if (rift_ipc_repeater_reply(c, what, msg)) {
        c->revision++;
        return 0;
    }
    error = cJSON_GetObjectItemCaseSensitive(msg, "error");
    if (cJSON_IsObject(error)) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(error, "message");

        const char *why = (cJSON_IsString(message) && message->valuestring)
                              ? message->valuestring
                              : "refused";

        c->errors_in++;
        snprintf(c->last_error, sizeof(c->last_error), "%s: %s",
                 method_of(what) ? method_of(what) : "meshcored", why);
        /* A refused send is the one error a reader is owed directly: they
         * typed the message and pressed the button, and the composer must
         * say what became of it rather than leaving it in the command
         * line's general error caption. */
        if (what == RIFT_REQ_SEND) {
            rift_model_send_failed(c->model, why);
        }
        /* The same for the requests a reader made by pressing a button: the
         * refusal is theirs to see, where they pressed it. */
        if (what == RIFT_REQ_ADVERT) {
            rift_model_action_failed(c->model, c->model->advert.kind, why, rift_mono_ms());
        } else if (what == RIFT_REQ_NODE_REMOVE || what == RIFT_REQ_NODE_RESET_PATH) {
            rift_model_action_failed(c->model, c->model->node_op.kind, why, rift_mono_ms());
        } else if (what == RIFT_REQ_CHANNEL_ADD || what == RIFT_REQ_CHANNEL_REMOVE ||
                   what == RIFT_REQ_SET_NAME || what == RIFT_REQ_SET_PATH_HASH) {
            rift_model_action_failed(c->model, c->model->manage_op.kind, why, rift_mono_ms());
        }
        /* A service with no path hash setting says so once, on connecting;
         * the screen then says the setting is not in this service. */
        if (what == RIFT_REQ_PATH_HASH) {
            c->model->path_hash_unsupported = 1;
            c->model->have_path_hash = 0;
        }
        /* A refused mesh.channels is still an answer, and for a screen it is
         * the same answer as an empty list: this service is not going to
         * list any channels. Leaving the list "not read yet" would make
         * COMMS say it is waiting for a service that is plainly replying -
         * which is what a build of meshcored older than this method would
         * produce.
         *
         * What it does NOT do is empty the list. A refusal says the service
         * would not answer, not that the channels are gone, and throwing
         * them away would take a joined channel off the screen because one
         * request was refused. mesh.nodes behaves the same way: a refused
         * snapshot leaves the nodes alone. Only a successful answer replaces
         * the list. */
        if (what == RIFT_REQ_CHANNELS) {
            c->model->channels_valid = 1;
        }
        c->revision++;
        return 0;
    }
    result = cJSON_GetObjectItemCaseSensitive(msg, "result");
    if (what == RIFT_REQ_NONE) {
        /* A reply to a request this client does not hold. Nothing is done
         * with it: acting on it would mean guessing which request it
         * answered. */
        c->bad_frames++;
        return 0;
    }
    switch (what) {
    case RIFT_REQ_INFO:
        rift_model_apply_info(c->model, result);
        break;
    case RIFT_REQ_STATUS:
        /* Stamped now rather than when the request went out: the model
         * derives which run of the service answered from this and the
         * uptime the reply carries, and the reply is the half that is
         * fresh. */
        rift_model_apply_status(c->model, result, rift_mono_ms());
        break;
    case RIFT_REQ_IDENTITY:
        rift_model_apply_identity(c->model, result);
        break;
    case RIFT_REQ_NODES:
        rift_model_apply_nodes(c->model, result);
        break;
    case RIFT_REQ_NODE:
        /* mesh.node answers one node in the shape mesh.nodes uses: an
         * update, never a second row - and not an event, so it is not
         * counted among the events that named the node. */
        rift_model_apply_node_reply(c->model, result);
        break;
    case RIFT_REQ_ADVERT:
        /* "accepted": queued for the dispatcher. Not transmitted - the
         * transmit's own outcome arrives as mesh.activity. */
        rift_model_action_done(c->model, c->model->advert.kind, rift_mono_ms());
        break;
    case RIFT_REQ_NODE_REMOVE: {
        const cJSON *node = cJSON_IsObject(result)
                                ? cJSON_GetObjectItemCaseSensitive(result, "node")
                                : NULL;
        const cJSON *key = cJSON_IsObject(node)
                               ? cJSON_GetObjectItemCaseSensitive(node, "public_key")
                               : NULL;

        /* The mesh.node event with reason "removed" takes the node off the
         * list as well; the answer does it too, so a client that is between
         * subscriptions does not go on showing a node the service has
         * forgotten. Only the node the answer names, and only when it is
         * the one that was asked about. */
        if (cJSON_IsString(key) && key->valuestring &&
            strcmp(key->valuestring, c->model->node_op.key) == 0) {
            rift_model_drop_node(c->model, key->valuestring);
        }
        rift_model_action_done(c->model, RIFT_ACTION_FORGET, rift_mono_ms());
        break;
    }
    case RIFT_REQ_NODE_RESET_PATH:
        /* The node afterwards, in the shape mesh.node answers. */
        rift_model_apply_node_reply(c->model, result);
        rift_model_action_done(c->model, RIFT_ACTION_RESET_PATH, rift_mono_ms());
        break;
    case RIFT_REQ_CHANNELS:
        rift_model_apply_channels(c->model, result);
        break;
    case RIFT_REQ_CHANNEL_ADD:
    case RIFT_REQ_CHANNEL_REMOVE:
        /* The mesh.channel event says the same to every subscriber; the list
         * is asked for again too, so a client between subscriptions is not
         * left showing the table as it was. */
        rift_model_action_done(c->model, c->model->manage_op.kind, rift_mono_ms());
        (void)rift_ipc_request_channels(c);
        break;
    case RIFT_REQ_SET_NAME:
        /* The identity afterwards. Each channel's text limit depends on the
         * name, which travels inside every channel payload, so the channel
         * list is read again as well. */
        rift_model_apply_identity(c->model, result);
        rift_model_action_done(c->model, RIFT_ACTION_RENAME, rift_mono_ms());
        /* Renamed is not saved. The service says whether the name was
         * written ("persisted"); when it was not, the rename holds only
         * until the service restarts, and the caption must not read as if
         * it were kept (rift_fmt_action). */
        if (c->model->manage_op.kind == RIFT_ACTION_RENAME && c->model->manage_op.done) {
            c->model->manage_op.unsaved =
                cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result, "persisted"));
        }
        (void)rift_ipc_request_channels(c);
        break;
    case RIFT_REQ_PATH_HASH:
        rift_model_apply_path_hash(c->model, result);
        break;
    case RIFT_REQ_SET_PATH_HASH:
        rift_model_apply_path_hash(c->model, result);
        rift_model_action_done(c->model, RIFT_ACTION_PATH_HASH, rift_mono_ms());
        break;
    case RIFT_REQ_MESSAGES:
        rift_model_apply_messages(c->model, result);
        break;
    case RIFT_REQ_SEND: {
        /* "accepted" is always true when there is a result at all
         * (docs/api/mesh.md); what matters here is the id, because that is
         * what the message itself will arrive under. The message is not
         * created from this reply: mesh.message carries it, and keying both
         * on the id is what keeps one message one row however they race. */
        const cJSON *id_of = cJSON_IsObject(result)
                                 ? cJSON_GetObjectItemCaseSensitive(result, "message_id")
                                 : NULL;
        const cJSON *route = cJSON_IsObject(result)
                                 ? cJSON_GetObjectItemCaseSensitive(result, "route")
                                 : NULL;

        rift_model_send_accepted(c->model,
                                 cJSON_IsNumber(id_of) ? (int64_t)id_of->valuedouble : 0,
                                 (cJSON_IsString(route) && route->valuestring)
                                     ? route->valuestring
                                     : NULL);
        break;
    }
    case RIFT_REQ_SUBSCRIBE:
        rift_rxlog_service_answered(c->rxlog, result, rift_mono_ms());
        break;
    case RIFT_REQ_UNSUBSCRIBE:
    case RIFT_REQ_NONE:
    default:
        break;
    }
    c->revision++;
    return 0;
}

static void pump(struct rift_ipc *c, int64_t now_ms)
{
    uint8_t buf[4096];
    int frames = 0;

    while (frames < RIFT_FRAMES_PER_POLL) {
        ssize_t r;
        int bad = 0;
        cJSON *msg;

        /* Everything already in the reader first: one read can carry many
         * frames, and a pass that read once and parsed once would fall
         * behind a burst it had already taken off the socket. */
        while (frames < RIFT_FRAMES_PER_POLL &&
               (msg = pocketipc_reader_next(&c->reader, &bad)) != NULL) {
            frames++;
            c->frames_in++;
            if (dispatch(c, msg) != 0) {
                cJSON_Delete(msg);
                return;
            }
            cJSON_Delete(msg);
        }
        if (bad) {
            /* A complete frame that was not JSON. The peer is speaking
             * something else; pocketipc says drop it. */
            drop(c, "meshcored sent a frame that is not JSON", now_ms);
            return;
        }
        if (frames >= RIFT_FRAMES_PER_POLL) {
            return;
        }
        r = read(c->fd, buf, sizeof(buf));
        if (r > 0) {
            if (pocketipc_reader_feed(&c->reader, buf, (size_t)r) != 0) {
                drop(c, "meshcored sent an oversized frame", now_ms);
                return;
            }
            continue;
        }
        if (r == 0) {
            drop(c, "meshcored closed the connection", now_ms);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return; /* nothing more waiting */
        }
        drop(c, "the connection to meshcored failed", now_ms);
        return;
    }
}

void rift_ipc_poll(struct rift_ipc *c, int64_t now_ms)
{
    if (!c || !c->model) {
        return;
    }
    if (c->fd < 0) {
        if (now_ms >= c->next_attempt_ms) {
            connect_now(c, now_ms);
        }
        return;
    }
    pump(c, now_ms);
    if (c->fd < 0) {
        return;
    }
    rift_ipc_repeater_poll(c, now_ms);
    /* What events do not carry. The service state does arrive as an event,
     * but only on a transition, so a client that never asked would show
     * nothing until something changed. */
    if (now_ms - c->last_status_ms >= RIFT_STATUS_PERIOD_MS) {
        c->last_status_ms = now_ms;
        if (request(c, RIFT_REQ_STATUS, NULL, now_ms) != 0) {
            return;
        }
    }
    if (now_ms - c->last_nodes_ms >= RIFT_NODES_PERIOD_MS) {
        c->last_nodes_ms = now_ms;
        if (request(c, RIFT_REQ_NODES, NULL, now_ms) != 0) {
            return;
        }
    }
    if (now_ms - c->last_channels_ms >= RIFT_CHANNELS_PERIOD_MS) {
        if (rift_ipc_request_channels(c) != 0) {
            return;
        }
    }
    if (now_ms - c->last_messages_ms >= RIFT_MESSAGES_PERIOD_MS) {
        if (rift_ipc_request_messages(c) != 0) {
            return;
        }
    }
}

void rift_ipc_close(struct rift_ipc *c)
{
    if (!c) {
        return;
    }
    if (c->fd >= 0) {
        /* Say so rather than merely going away. Disconnecting clears the
         * subscription too (docs/api/mesh.md), so this is politeness with
         * a purpose: the service stops writing to a socket nobody is
         * reading before it learns that nobody is. The reply is never
         * read, which is why it is not remembered as pending. */
        if (c->subscribed) {
            (void)request(c, RIFT_REQ_UNSUBSCRIBE, NULL, rift_mono_ms());
        }
        if (c->fd >= 0) {
            close(c->fd);
            c->fd = -1;
        }
        c->subscribed = 0;
    }
    forget_pending(c);
    pocketipc_reader_free(&c->reader);
    pocketipc_reader_init(&c->reader);
}

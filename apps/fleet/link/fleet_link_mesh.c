/*
 * PocketFleet's link to the mesh. See fleet_link_mesh.h.
 *
 * The shape is RIFT's meshcored client (apps/rift/rift_ipc.h), for the same
 * reasons: nothing here may stop the LVGL thread, so requests are written and
 * forgotten, replies are matched by id when they turn up, the socket is read
 * non-blocking in bounded passes, and the only wait in the module is a
 * connect bounded well inside one frame. And one connection carries both the
 * subscription and the requests, because pocketipc_call would drop the
 * events that arrive while it waits.
 *
 * What it asks meshcored: mesh.subscribe, mesh.identity (our key), mesh.status
 * (whether the radio is usable, and which run of the service this is),
 * mesh.nodes (who could be invited), mesh.app_inbox (datagrams that arrived
 * while nobody was listening) and mesh.app_send. And mesh.advert, zero-hop,
 * only from advertise(), which only a button reaches.
 *
 * What it listens to: mesh.app on FLEET_MESH_PORT, mesh.node, mesh.state.
 *
 * Received datagrams are taken exactly once, by id. The ids are the
 * service's, per run: when mesh.status reports another run_id, the cursor
 * starts again from 0. While the catch-up for a connection is outstanding,
 * mesh.app events are ignored - the stream is ordered, so any datagram whose
 * event arrives before the inbox answer is in that answer - and after it an
 * event is taken only when its id is past the cursor.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_link_mesh.h"

#include "pocketipc/pocketipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The same bounds as RIFT's client, for the same reasons. */
#define CONNECT_TIMEOUT_MS 120
#define BACKOFF_MIN_MS 500
#define BACKOFF_MAX_MS 5000
#define FRAMES_PER_POLL 64
#define MAX_PENDING 16
/* What events do not carry, asked for again: the radio's state and the run,
 * the node list, and the inbox as a backstop for an event that was lost. */
#define STATUS_PERIOD_MS 2000
#define NODES_PERIOD_MS 20000
#define INBOX_PERIOD_MS 30000
/* A datagram older than this when this client first sees it - one that sat in
 * the service's inbox while Fleet was closed - is not handed on. The match
 * would handle it (every packet names its session), but an invitation from a
 * quarter of an hour ago is not one to show. */
#define STALE_MS (15 * 60 * 1000)
#define RX_QUEUE 16
/* Players held, not nodes heard: the service lists every node, but only those
 * that could play are kept (node_apply), and when more than this many could,
 * the most recently heard are. The lobby shows far fewer. */
#define NODES_MAX 64
#define ADV_TYPE_CHAT 1              /* MeshCore's advert type for a companion (DOCUMENTED:
                                      * vendor/RIFT/src/helpers/AdvertDataHelpers.h) */

enum req {
    REQ_NONE = 0,
    REQ_SUBSCRIBE,
    REQ_UNSUBSCRIBE,
    REQ_IDENTITY,
    REQ_STATUS,
    REQ_NODES,
    REQ_INBOX,
    REQ_SEND,
    REQ_ADVERT,
};

struct pending {
    int id;
    enum req what;
};

struct rx {
    uint8_t from[FLEET_KEY_BYTES];
    uint8_t bytes[FLEET_PROTO_MAX];
    size_t n;
};

struct node {
    uint8_t key[FLEET_KEY_BYTES];
    char name[FLEET_LINK_NAME_MAX];
    int64_t heard_ms;
    int hops;
};

struct mesh {
    struct fleet_link link;
    char service[32];

    int fd;
    struct pocketipc_reader reader;
    int next_id;
    struct pending pending[MAX_PENDING];
    int attempts;                    /* connects tried and failed since the last success */
    int64_t next_attempt_ms;
    int backoff_ms;
    int64_t last_status_ms;
    int64_t last_nodes_ms;
    int64_t last_inbox_ms;
    int64_t now;

    int have_key;
    uint8_t key[FLEET_KEY_BYTES];
    int have_status;
    int online;
    char run_id[40];
    uint64_t cursor;                 /* the last datagram id taken, this run */
    int catching_up;                 /* an inbox request for this connection is out */
    int subscribed;

    struct rx rx[RX_QUEUE];
    int rx_head;
    int rx_count;

    struct node nodes[NODES_MAX];
    int node_count;

    uint32_t retry_base;

    /* For a test, and for anyone reading a core dump. */
    unsigned connects;
    unsigned sends;
    unsigned send_errors;
    unsigned taken;
    unsigned dropped;
    unsigned ignored;
};

/* ---- small helpers -------------------------------------------------------- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Decode exactly-even hex into at most max bytes. Returns the count, or -1. */
static int unhex(const char *s, uint8_t *out, size_t max)
{
    size_t len;
    size_t i;

    if (!s) {
        return -1;
    }
    len = strlen(s);
    if (len % 2 != 0 || len / 2 > max) {
        return -1;
    }
    for (i = 0; i < len / 2; i++) {
        int hi = hexval(s[2 * i]);
        int lo = hexval(s[2 * i + 1]);

        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return (int)(len / 2);
}

static void tohex(const uint8_t *b, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < n; i++) {
        out[2 * i] = digits[b[i] >> 4];
        out[2 * i + 1] = digits[b[i] & 15];
    }
    out[2 * n] = '\0';
}

static int key_of(const cJSON *o, const char *field, uint8_t key[FLEET_KEY_BYTES])
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, field);

    return cJSON_IsString(v) && unhex(v->valuestring, key, FLEET_KEY_BYTES) == FLEET_KEY_BYTES
               ? 0
               : -1;
}

static const char *method_of(enum req what)
{
    switch (what) {
    case REQ_SUBSCRIBE: return "mesh.subscribe";
    case REQ_UNSUBSCRIBE: return "mesh.unsubscribe";
    case REQ_IDENTITY: return "mesh.identity";
    case REQ_STATUS: return "mesh.status";
    case REQ_NODES: return "mesh.nodes";
    case REQ_INBOX: return "mesh.app_inbox";
    case REQ_SEND: return "mesh.app_send";
    case REQ_ADVERT: return "mesh.advert";
    case REQ_NONE:
    default: return NULL;
    }
}

/* ---- the connection ------------------------------------------------------- */

static void backoff(struct mesh *m, int64_t now)
{
    m->next_attempt_ms = now + m->backoff_ms;
    m->backoff_ms *= 2;
    if (m->backoff_ms > BACKOFF_MAX_MS) {
        m->backoff_ms = BACKOFF_MAX_MS;
    }
}

static void drop(struct mesh *m, int64_t now)
{
    if (m->fd >= 0) {
        close(m->fd);
        m->fd = -1;
    }
    memset(m->pending, 0, sizeof(m->pending));
    pocketipc_reader_free(&m->reader);
    pocketipc_reader_init(&m->reader);
    m->have_status = 0;
    m->online = 0;
    m->catching_up = 0;
    m->subscribed = 0;
    m->attempts++;
    backoff(m, now);
}

static int remember(struct mesh *m, int id, enum req what)
{
    int i;

    for (i = 0; i < MAX_PENDING; i++) {
        if (m->pending[i].what == REQ_NONE) {
            m->pending[i].id = id;
            m->pending[i].what = what;
            return 0;
        }
    }
    return -1;
}

static enum req take_pending(struct mesh *m, int id)
{
    int i;

    for (i = 0; i < MAX_PENDING; i++) {
        if (m->pending[i].what != REQ_NONE && m->pending[i].id == id) {
            enum req what = m->pending[i].what;

            m->pending[i].what = REQ_NONE;
            return what;
        }
    }
    return REQ_NONE;
}

/* Write one request; params is consumed. 0, or -1: no connection, no room,
 * or the write failed (and the connection is gone). */
static int request(struct mesh *m, enum req what, cJSON *params)
{
    cJSON *msg;
    int id;
    int rc;

    if (m->fd < 0) {
        cJSON_Delete(params);
        return -1;
    }
    id = m->next_id++;
    if (m->next_id <= 0) {
        m->next_id = 1;
    }
    if (what != REQ_UNSUBSCRIBE && remember(m, id, what) != 0) {
        cJSON_Delete(params);
        return -1;
    }
    msg = cJSON_CreateObject();
    if (!msg) {
        take_pending(m, id);
        cJSON_Delete(params);
        return -1;
    }
    cJSON_AddNumberToObject(msg, "id", id);
    cJSON_AddStringToObject(msg, "method", method_of(what));
    if (params) {
        cJSON_AddItemToObject(msg, "params", params);
    }
    rc = pocketipc_send(m->fd, msg);
    cJSON_Delete(msg);
    if (rc != 0) {
        drop(m, m->now);
        return -1;
    }
    return 0;
}

static int ask_inbox(struct mesh *m)
{
    cJSON *p = cJSON_CreateObject();

    if (!p) {
        return -1;
    }
    cJSON_AddNumberToObject(p, "port", FLEET_MESH_PORT);
    cJSON_AddNumberToObject(p, "after_id", (double)m->cursor);
    m->last_inbox_ms = m->now;
    return request(m, REQ_INBOX, p);
}

static void connect_now(struct mesh *m, int64_t now)
{
    int fd = pocketipc_connect_timeout(m->service, CONNECT_TIMEOUT_MS);

    if (fd < 0) {
        m->attempts++;
        backoff(m, now);
        return;
    }
    m->fd = fd;
    m->connects++;
    m->attempts = 0;
    m->backoff_ms = BACKOFF_MIN_MS;
    pocketipc_reader_free(&m->reader);
    pocketipc_reader_init(&m->reader);
    memset(m->pending, 0, sizeof(m->pending));
    /* Subscribe first, so nothing is missed between the snapshot and the
     * events; the inbox follows the status answer, which says which run this
     * is and so where the cursor starts. */
    m->catching_up = 1;
    if (request(m, REQ_SUBSCRIBE, NULL) != 0) {
        return;
    }
    m->subscribed = 1;
    if (request(m, REQ_IDENTITY, NULL) != 0) {
        return;
    }
    m->last_status_ms = now;
    if (request(m, REQ_STATUS, NULL) != 0) {
        return;
    }
    m->last_nodes_ms = now;
    (void)request(m, REQ_NODES, NULL);
}

/* ---- what the service tells us -------------------------------------------- */

static struct node *node_find(struct mesh *m, const uint8_t key[FLEET_KEY_BYTES])
{
    int i;

    for (i = 0; i < m->node_count; i++) {
        if (memcmp(m->nodes[i].key, key, FLEET_KEY_BYTES) == 0) {
            return &m->nodes[i];
        }
    }
    return NULL;
}

/* A slot for a player heard at heard_ms: a free one, else the least recently
 * heard player's when this one was heard later, else NULL. */
static struct node *node_slot(struct mesh *m, int64_t heard_ms)
{
    struct node *oldest = NULL;
    int i;

    if (m->node_count < NODES_MAX) {
        return &m->nodes[m->node_count++];
    }
    for (i = 0; i < m->node_count; i++) {
        if (!oldest || m->nodes[i].heard_ms < oldest->heard_ms) {
            oldest = &m->nodes[i];
        }
    }
    return oldest->heard_ms < heard_ms ? oldest : NULL;
}

static void node_apply(struct mesh *m, const cJSON *o)
{
    uint8_t key[FLEET_KEY_BYTES];
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(o, "name");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(o, "type");
    const cJSON *hops = cJSON_GetObjectItemCaseSensitive(o, "hops");
    const cJSON *heard = cJSON_GetObjectItemCaseSensitive(o, "last_heard_mono_ms");
    /* meshcored and this app read the same CLOCK_MONOTONIC on one device. */
    int64_t heard_ms = cJSON_IsNumber(heard) ? (int64_t)heard->valuedouble : 0;
    struct node *n;

    if (!cJSON_IsObject(o) || key_of(o, "public_key", key) != 0) {
        return;
    }
    n = node_find(m, key);
    /* Only a node that could play is kept: a repeater or a room server cannot,
     * nor can our own key. Filtered here, before the table's bound, so however
     * many of those the service has heard they never take a player's place. */
    if (!cJSON_IsNumber(type) || type->valueint != ADV_TYPE_CHAT ||
        (m->have_key && memcmp(key, m->key, FLEET_KEY_BYTES) == 0)) {
        if (n) {
            *n = m->nodes[--m->node_count];
        }
        return;
    }
    if (!n) {
        if (!(n = node_slot(m, heard_ms))) {
            return;
        }
        memset(n, 0, sizeof(*n));
        memcpy(n->key, key, FLEET_KEY_BYTES);
    }
    /* The name is already sanitised by the service (docs/api/mesh.md,
     * "Remote text"); it is copied, cut to fit, and never interpreted. */
    snprintf(n->name, sizeof(n->name), "%s",
             cJSON_IsString(name) && name->valuestring ? name->valuestring : "");
    n->hops = cJSON_IsNumber(hops) ? hops->valueint : -1;
    n->heard_ms = heard_ms;
}

static void node_forget(struct mesh *m, const cJSON *o)
{
    uint8_t key[FLEET_KEY_BYTES];
    struct node *n;

    if (key_of(o, "public_key", key) != 0 || !(n = node_find(m, key))) {
        return;
    }
    *n = m->nodes[--m->node_count];
}

static void apply_nodes(struct mesh *m, const cJSON *result)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(result, "nodes");
    const cJSON *item;

    if (!cJSON_IsArray(arr)) {
        return;
    }
    /* A snapshot replaces the list, so a node the service forgot goes. */
    m->node_count = 0;
    cJSON_ArrayForEach (item, arr) {
        node_apply(m, item);
    }
}

static void apply_status(struct mesh *m, const cJSON *result)
{
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(result, "state");
    const cJSON *radio = cJSON_GetObjectItemCaseSensitive(result, "radio");
    const cJSON *online = cJSON_GetObjectItemCaseSensitive(radio, "online");
    const cJSON *run = cJSON_GetObjectItemCaseSensitive(result, "run_id");
    const char *run_id = cJSON_IsString(run) && run->valuestring ? run->valuestring : "";

    m->have_status = 1;
    m->online = cJSON_IsTrue(online) && cJSON_IsString(state) &&
                strcmp(state->valuestring, "online") == 0;
    if (strcmp(run_id, m->run_id) != 0) {
        /* Another run of the service: its ids start again from 1. */
        snprintf(m->run_id, sizeof(m->run_id), "%s", run_id);
        m->cursor = 0;
        m->catching_up = 1;
    }
    if (m->catching_up) {
        (void)ask_inbox(m);
    }
}

/* One datagram, from an event or the inbox. */
static void take_datagram(struct mesh *m, const cJSON *dg)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(dg, "id");
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(dg, "port");
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(dg, "payload_hex");
    const cJSON *at = cJSON_GetObjectItemCaseSensitive(dg, "mono_ms");
    struct rx r;
    int n;
    uint64_t v;

    if (!cJSON_IsNumber(id) || id->valuedouble < 1 || !cJSON_IsNumber(port) ||
        port->valueint != FLEET_MESH_PORT) {
        return;
    }
    v = (uint64_t)id->valuedouble;
    if (v <= m->cursor) {
        return;                                  /* taken already */
    }
    m->cursor = v;
    if (key_of(dg, "from", r.from) != 0 || !cJSON_IsString(payload) ||
        (n = unhex(payload->valuestring, r.bytes, sizeof(r.bytes))) <= 0) {
        m->ignored++;                            /* not a Fleet packet: too long, or not hex */
        return;
    }
    if (cJSON_IsNumber(at) && m->now - (int64_t)at->valuedouble > STALE_MS) {
        m->ignored++;
        return;
    }
    r.n = (size_t)n;
    if (m->rx_count == RX_QUEUE) {
        /* The session drains 32 a poll; this is a burst it has not reached.
         * The newest is kept - the protocol retransmits, and the newest
         * carries the most. */
        m->rx_head = (m->rx_head + 1) % RX_QUEUE;
        m->rx_count--;
        m->dropped++;
    }
    m->rx[(m->rx_head + m->rx_count) % RX_QUEUE] = r;
    m->rx_count++;
    m->taken++;
}

static void apply_inbox(struct mesh *m, const cJSON *result)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(result, "datagrams");
    const cJSON *item;

    m->catching_up = 0;
    if (!cJSON_IsArray(arr)) {
        return;
    }
    cJSON_ArrayForEach (item, arr) {
        take_datagram(m, item);
    }
}

static void on_event(struct mesh *m, const char *name, const cJSON *data)
{
    if (strcmp(name, "mesh.app") == 0) {
        if (!m->catching_up) {
            take_datagram(m, cJSON_GetObjectItemCaseSensitive(data, "datagram"));
        }
    } else if (strcmp(name, "mesh.node") == 0) {
        const cJSON *reason = cJSON_GetObjectItemCaseSensitive(data, "reason");
        const cJSON *node = cJSON_GetObjectItemCaseSensitive(data, "node");

        if (cJSON_IsString(reason) && strcmp(reason->valuestring, "removed") == 0) {
            node_forget(m, node);
        } else {
            node_apply(m, node);
        }
    } else if (strcmp(name, "mesh.state") == 0) {
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(data, "state");

        /* Only the state word: radio.online follows it, and the next status
         * answer confirms either way. */
        if (cJSON_IsString(state)) {
            m->online = strcmp(state->valuestring, "online") == 0;
        }
    }
}

static void on_reply(struct mesh *m, enum req what, const cJSON *result, int failed)
{
    if (failed) {
        if (what == REQ_SEND) {
            /* Refused after the fact (radio gone, node not held). The match
             * has it as sent and retries on its own timer, which is the
             * behaviour a lost packet gets; nothing more is owed here. */
            m->send_errors++;
        } else if (what == REQ_INBOX) {
            m->catching_up = 0;
        }
        return;
    }
    switch (what) {
    case REQ_IDENTITY:
        if (key_of(result, "public_key", m->key) == 0) {
            m->have_key = 1;
        }
        break;
    case REQ_STATUS:
        apply_status(m, result);
        break;
    case REQ_NODES:
        apply_nodes(m, result);
        break;
    case REQ_INBOX:
        apply_inbox(m, result);
        break;
    case REQ_SEND: {
        const cJSON *est = cJSON_GetObjectItemCaseSensitive(result, "est_timeout_ms");

        if (cJSON_IsNumber(est) && est->valuedouble > 0 && est->valuedouble < 600000) {
            m->retry_base = (uint32_t)est->valuedouble;
        }
        break;
    }
    default:
        break;
    }
}

static void dispatch(struct mesh *m, const cJSON *msg)
{
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(msg, "event");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    enum req what;

    if (cJSON_IsString(event) && event->valuestring) {
        on_event(m, event->valuestring, cJSON_GetObjectItemCaseSensitive(msg, "data"));
        return;
    }
    if (!cJSON_IsNumber(id)) {
        return;
    }
    what = take_pending(m, (int)id->valuedouble);
    if (what == REQ_NONE) {
        return;
    }
    on_reply(m, what, cJSON_GetObjectItemCaseSensitive(msg, "result"),
             cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(msg, "error")));
}

static void pump(struct mesh *m)
{
    uint8_t buf[4096];
    int frames = 0;

    while (frames < FRAMES_PER_POLL && m->fd >= 0) {
        cJSON *msg;
        int bad = 0;
        ssize_t r;

        while (frames < FRAMES_PER_POLL && (msg = pocketipc_reader_next(&m->reader, &bad))) {
            frames++;
            dispatch(m, msg);
            cJSON_Delete(msg);
            if (m->fd < 0) {
                return;
            }
        }
        if (bad) {
            drop(m, m->now);
            return;
        }
        if (frames >= FRAMES_PER_POLL) {
            return;
        }
        r = read(m->fd, buf, sizeof(buf));
        if (r > 0) {
            if (pocketipc_reader_feed(&m->reader, buf, (size_t)r) != 0) {
                drop(m, m->now);
                return;
            }
            continue;
        }
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        drop(m, m->now);
        return;
    }
}

/* ---- the link ops ------------------------------------------------------------ */

static void m_poll(void *ctx, int64_t now)
{
    struct mesh *m = ctx;

    m->now = now;
    if (m->fd < 0) {
        if (now >= m->next_attempt_ms) {
            connect_now(m, now);
        }
        return;
    }
    pump(m);
    if (m->fd < 0) {
        return;
    }
    if (now - m->last_status_ms >= STATUS_PERIOD_MS) {
        m->last_status_ms = now;
        if (request(m, REQ_STATUS, NULL) != 0) {
            return;
        }
    }
    if (now - m->last_nodes_ms >= NODES_PERIOD_MS) {
        m->last_nodes_ms = now;
        if (request(m, REQ_NODES, NULL) != 0) {
            return;
        }
    }
    if (!m->catching_up && m->have_status && now - m->last_inbox_ms >= INBOX_PERIOD_MS) {
        (void)ask_inbox(m);
    }
}

static enum fleet_link_state m_state(void *ctx)
{
    struct mesh *m = ctx;

    if (m->fd < 0) {
        return m->attempts > 0 ? FLEET_LINK_NO_SERVICE : FLEET_LINK_CONNECTING;
    }
    if (!m->have_status || !m->have_key) {
        return FLEET_LINK_CONNECTING;
    }
    return m->online ? FLEET_LINK_UP : FLEET_LINK_RADIO_OFF;
}

static enum fleet_link_send m_send(void *ctx, const uint8_t to[FLEET_KEY_BYTES],
                                   const uint8_t *buf, size_t n)
{
    struct mesh *m = ctx;
    char key[FLEET_KEY_BYTES * 2 + 1];
    char payload[FLEET_PROTO_MAX * 2 + 1];
    cJSON *p;

    if (!buf || n == 0 || n > FLEET_PROTO_MAX) {
        return FLEET_LINK_FAILED;
    }
    if (m->fd < 0) {
        return FLEET_LINK_FAILED;
    }
    /* The service is there but cannot transmit: nothing goes out, and the
     * match holds its attempt rather than spending it. */
    if (m_state(m) != FLEET_LINK_UP) {
        return FLEET_LINK_BUSY;
    }
    p = cJSON_CreateObject();
    if (!p) {
        return FLEET_LINK_BUSY;
    }
    tohex(to, FLEET_KEY_BYTES, key);
    tohex(buf, n, payload);
    cJSON_AddStringToObject(p, "to", key);
    cJSON_AddNumberToObject(p, "port", FLEET_MESH_PORT);
    cJSON_AddStringToObject(p, "payload_hex", payload);
    if (request(m, REQ_SEND, p) != 0) {
        return m->fd < 0 ? FLEET_LINK_FAILED : FLEET_LINK_BUSY;
    }
    m->sends++;
    return FLEET_LINK_SENT;
}

static int m_recv(void *ctx, uint8_t from[FLEET_KEY_BYTES], uint8_t *buf, size_t *n)
{
    struct mesh *m = ctx;
    const struct rx *r;

    if (m->rx_count == 0) {
        return 0;
    }
    r = &m->rx[m->rx_head];
    memcpy(from, r->from, FLEET_KEY_BYTES);
    memcpy(buf, r->bytes, r->n);
    *n = r->n;
    m->rx_head = (m->rx_head + 1) % RX_QUEUE;
    m->rx_count--;
    return 1;
}

static int m_self(void *ctx, uint8_t key[FLEET_KEY_BYTES])
{
    struct mesh *m = ctx;

    if (!m->have_key) {
        return -1;
    }
    memcpy(key, m->key, FLEET_KEY_BYTES);
    return 0;
}

static int m_peers(void *ctx, struct fleet_link_peer *out, int max)
{
    struct mesh *m = ctx;
    int count = 0;
    int i;

    for (i = 0; i < m->node_count; i++) {
        const struct node *nd = &m->nodes[i];
        int j;

        /* Only players are kept (node_apply); our own key is refused again
         * here in case it was listed before the service told us which it is. */
        if (m->have_key && memcmp(nd->key, m->key, FLEET_KEY_BYTES) == 0) {
            continue;
        }
        /* Insertion into a list kept most recently heard first. */
        j = count < max ? count : max - 1;
        if (count >= max && out[j].heard_ms >= nd->heard_ms) {
            continue;
        }
        while (j > 0 && out[j - 1].heard_ms < nd->heard_ms) {
            out[j] = out[j - 1];
            j--;
        }
        memcpy(out[j].key, nd->key, FLEET_KEY_BYTES);
        snprintf(out[j].name, sizeof(out[j].name), "%s", nd->name);
        out[j].heard_ms = nd->heard_ms;
        out[j].hops = nd->hops;
        if (count < max) {
            count++;
        }
    }
    return count;
}

static const char *m_name(void *ctx, const uint8_t key[FLEET_KEY_BYTES])
{
    struct mesh *m = ctx;
    const struct node *n = node_find(m, key);

    return n && n->name[0] ? n->name : NULL;
}

static int m_advert(void *ctx)
{
    struct mesh *m = ctx;
    cJSON *p;

    if (m->fd < 0 || m_state(m) != FLEET_LINK_UP) {
        return -1;
    }
    p = cJSON_CreateObject();
    if (!p) {
        return -1;
    }
    /* Zero-hop: heard by those in range, repeated by nobody (docs/api/mesh.md). */
    cJSON_AddBoolToObject(p, "zero_hop", 1);
    return request(m, REQ_ADVERT, p);
}

static uint32_t m_base(void *ctx)
{
    return ((struct mesh *)ctx)->retry_base;
}

static void m_close(void *ctx)
{
    struct mesh *m = ctx;

    if (m->fd >= 0) {
        if (m->subscribed) {
            (void)request(m, REQ_UNSUBSCRIBE, NULL);
        }
        if (m->fd >= 0) {
            close(m->fd);
        }
    }
    pocketipc_reader_free(&m->reader);
    free(m);
}

static const struct fleet_link_ops OPS = {
    m_poll, m_send, m_recv, m_self, m_state, m_peers, m_name, m_advert, m_base, m_close,
};

struct fleet_link *fleet_link_mesh_open(const char *service)
{
    struct mesh *m = calloc(1, sizeof(*m));

    if (!m) {
        return NULL;
    }
    snprintf(m->service, sizeof(m->service), "%s", service ? service : FLEET_MESH_SERVICE);
    m->fd = -1;
    m->next_id = 1;
    m->backoff_ms = BACKOFF_MIN_MS;
    pocketipc_reader_init(&m->reader);
    m->link.ops = &OPS;
    m->link.ctx = m;
    return &m->link;
}

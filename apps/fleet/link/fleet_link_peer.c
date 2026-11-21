/*
 * PocketFleet's link to the local network. See fleet_link_peer.h.
 *
 * The shape is fleet_link_mesh.c's, for the same reasons: nothing here may
 * stop the LVGL thread, so requests are written and forgotten, replies are
 * matched by id when they turn up, and the socket is read non-blocking in
 * bounded passes. The waits that remain are bounded: pocketipc's own - a
 * connect of at most CONNECT_TIMEOUT_MS, and a write to a peerd that has
 * stopped reading, which pocketipc gives up on and the link then drops -
 * and one of at most 60 ms when the link is closed (p_close). One connection carries both the subscription and the
 * requests.
 *
 * What it asks peerd: peer.subscribe, peer.identity (our key), peer.status
 * (whether it is ready, which run this is, our address), peer.set_visible
 * (the lease), peer.nodes (who could be invited), peer.app_inbox (datagrams
 * that arrived while nobody was listening), peer.app_send, peer.kept /
 * peer.keep / peer.release (the opponent of the match in hand), and
 * peer.probe / peer.probes (a device at an address).
 *
 * What it listens to: peer.app on FLEET_PEER_PORT.
 *
 * Received datagrams are taken exactly once, by id, as on the mesh: the ids
 * are the service's, per run; when peer.status reports another run_id the
 * cursor starts again from 0; while the catch-up for a connection is
 * outstanding events are ignored, and after it an event is taken only when
 * its id is past the cursor.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fleet_link_peer.h"

#include "pocketipc/pocketipc.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The same bounds as the mesh link, for the same reasons. */
#define CONNECT_TIMEOUT_MS 120
#define BACKOFF_MIN_MS 500
#define BACKOFF_MAX_MS 5000
#define FRAMES_PER_POLL 64
#define MAX_PENDING 24
#define STATUS_PERIOD_MS 2000
/* peerd raises no event for a node: the list is asked for. It is a local
 * request and the lobby wants a player to appear within a breath. */
#define NODES_PERIOD_MS 2000
#define INBOX_PERIOD_MS 30000
/* peerd's lease is 30 s (docs/api/peer.md); renewed three times within it,
 * so two lost requests in a row still leave it standing. */
#define LEASE_RENEW_MS 10000
/* The kept list is checked again this often, so a record peerd lost to a
 * fault is asserted again (ADR-017, section 5: reconcile both ways). */
#define KEPT_PERIOD_MS 15000
#define PROBES_PERIOD_MS 300
#define STALE_MS (15 * 60 * 1000)
/* More than one catch-up can hold (peerd's inbox is 32) and one pass of
 * events can bring (FRAMES_PER_POLL), so nothing is pushed out before the
 * session has had a chance to take it. */
#define RX_QUEUE 96
#define NODES_MAX 32            /* peerd holds no more */
#define KEPT_MAX 8              /* peerd keeps 4; read with room to spare */
/* Closing: how long peerd's answers to what was last asked are waited for,
 * in all, before the request to hide is written (p_close). */
#define CLOSE_QUIET_MS 15
#define CLOSE_ROUNDS 4

enum req {
    REQ_NONE = 0,
    REQ_SUBSCRIBE,
    REQ_UNSUBSCRIBE,
    REQ_IDENTITY,
    REQ_STATUS,
    REQ_VISIBLE,
    REQ_HIDE,
    REQ_NODES,
    REQ_INBOX,
    REQ_SEND,
    REQ_KEPT,
    REQ_KEEP,
    REQ_RELEASE,
    REQ_PROBE,
    REQ_PROBES,
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
    int proved;
};

struct peer {
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
    int64_t last_lease_ms;
    int64_t last_kept_ms;
    int64_t last_probes_ms;
    int64_t now;

    int have_key;
    uint8_t key[FLEET_KEY_BYTES];
    int have_status;
    int ready;                       /* peerd's state is "ready" */
    int socket;                      /* peerd has its UDP socket bound */
    int interfaces;                  /* how many it announces on */
    int no_net_seen;                 /* statuses in a row with nothing to announce on */
    int visible;
    int want_visible;                /* the owner wants the device seen (the default) */
    int hide_due;                    /* it stopped wanting it: the lease is to be ended */
    int lease_asked;                 /* a lease has been asked for on this connection */
    char fault[24];
    char address[16];
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

    /* The opponent of the match in hand, as the session last said. Nothing
     * is kept or released before it has said (match_known). */
    int match_known;
    int have_match;
    uint8_t match_key[FLEET_KEY_BYTES];
    int kept_stale;                  /* peerd's list is to be read and put right */
    int kept_count;                  /* as last listed; -1 before that */
    int kept_wait_ms;                /* how long before a stale list is read again */

    enum fleet_link_peer_probe_state probe;
    long probe_id;
    int probe_req;                   /* the id of the peer.probe request that is ours */
    int probe_listed_from;           /* peer.probes requests after this id can show it */
    int last_id;                     /* the id of the request last written */
    uint8_t probe_key[FLEET_KEY_BYTES];

    /* For a test, and for anyone reading a core dump. */
    unsigned connects;
    unsigned sends;
    unsigned send_errors;
    unsigned taken;
    unsigned dropped;
    unsigned ignored;
    unsigned leases;
    unsigned keeps;
    unsigned releases;
};

static const struct fleet_link_ops OPS;

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

/* Dotted IPv4 and nothing else: what peerd itself accepts. Refused here so
 * that whatever a keyboard produced never becomes a request. */
static int address_ok(const char *s)
{
    int part;

    if (!s) {
        return 0;
    }
    for (part = 0; part < 4; part++) {
        unsigned v = 0;
        int digits = 0;

        while (*s >= '0' && *s <= '9' && digits < 3) {
            v = v * 10 + (unsigned)(*s - '0');
            s++;
            digits++;
        }
        if (digits == 0 || v > 255 || *s != (part < 3 ? '.' : '\0')) {
            return 0;
        }
        s++;
    }
    return 1;
}

static const char *method_of(enum req what)
{
    switch (what) {
    case REQ_SUBSCRIBE: return "peer.subscribe";
    case REQ_UNSUBSCRIBE: return "peer.unsubscribe";
    case REQ_IDENTITY: return "peer.identity";
    case REQ_STATUS: return "peer.status";
    case REQ_VISIBLE:
    case REQ_HIDE: return "peer.set_visible";
    case REQ_NODES: return "peer.nodes";
    case REQ_INBOX: return "peer.app_inbox";
    case REQ_SEND: return "peer.app_send";
    case REQ_KEPT: return "peer.kept";
    case REQ_KEEP: return "peer.keep";
    case REQ_RELEASE: return "peer.release";
    case REQ_PROBE: return "peer.probe";
    case REQ_PROBES: return "peer.probes";
    case REQ_NONE:
    default: return NULL;
    }
}

/* ---- the connection ------------------------------------------------------- */

static void backoff(struct peer *m, int64_t now)
{
    m->next_attempt_ms = now + m->backoff_ms;
    m->backoff_ms *= 2;
    if (m->backoff_ms > BACKOFF_MAX_MS) {
        m->backoff_ms = BACKOFF_MAX_MS;
    }
}

static void drop(struct peer *m, int64_t now)
{
    if (m->fd >= 0) {
        close(m->fd);
        m->fd = -1;
    }
    memset(m->pending, 0, sizeof(m->pending));
    pocketipc_reader_free(&m->reader);
    pocketipc_reader_init(&m->reader);
    m->have_status = 0;
    m->ready = 0;
    m->socket = 0;
    m->interfaces = 0;
    m->no_net_seen = 0;
    m->visible = 0;
    m->hide_due = 0;                 /* a lease nobody renews ends by itself */
    m->lease_asked = 0;
    m->catching_up = 0;
    m->subscribed = 0;
    m->kept_stale = 1;               /* whatever comes back may have lost its list */
    if (m->probe == FLEET_PEER_PROBE_PENDING) {
        m->probe = FLEET_PEER_PROBE_NO_ANSWER;
    }
    m->attempts++;
    backoff(m, now);
}

static int remember(struct peer *m, int id, enum req what)
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

static enum req take_pending(struct peer *m, int id)
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
static int request(struct peer *m, enum req what, cJSON *params)
{
    cJSON *msg;
    int untracked = what == REQ_UNSUBSCRIBE || what == REQ_HIDE;
    int id;
    int rc;

    if (m->fd < 0) {
        cJSON_Delete(params);
        return -1;
    }
    id = m->next_id;
    m->next_id = id >= 0x3fffffff ? 1 : id + 1;
    m->last_id = id;
    if (!untracked && remember(m, id, what) != 0) {
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

static int ask_inbox(struct peer *m)
{
    cJSON *p = cJSON_CreateObject();

    if (!p) {
        return -1;
    }
    cJSON_AddNumberToObject(p, "port", FLEET_PEER_PORT);
    cJSON_AddNumberToObject(p, "after_id", (double)m->cursor);
    m->last_inbox_ms = m->now;
    return request(m, REQ_INBOX, p);
}

/* Renew the visibility lease. While this link is polled, Fleet is in
 * multiplayer and the device may be seen; nothing else ever asks for it. */
static int ask_visible(struct peer *m)
{
    cJSON *p = cJSON_CreateObject();

    if (!p) {
        return -1;
    }
    cJSON_AddBoolToObject(p, "visible", 1);
    m->last_lease_ms = m->now;
    m->lease_asked = 1;
    m->leases++;
    return request(m, REQ_VISIBLE, p);
}

static int ask_key(struct peer *m, enum req what, const uint8_t key[FLEET_KEY_BYTES])
{
    char hex[FLEET_KEY_BYTES * 2 + 1];
    cJSON *p = cJSON_CreateObject();

    if (!p) {
        return -1;
    }
    tohex(key, FLEET_KEY_BYTES, hex);
    cJSON_AddStringToObject(p, "key", hex);
    return request(m, what, p);
}

static void connect_now(struct peer *m, int64_t now)
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
    m->kept_stale = 1;
    m->kept_wait_ms = STATUS_PERIOD_MS;     /* a new connection: no back-off carried over */
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
    if (m->want_visible && ask_visible(m) != 0) {
        return;
    }
    m->last_nodes_ms = now;
    (void)request(m, REQ_NODES, NULL);
}

/* ---- what the service tells us -------------------------------------------- */

static struct node *node_find(struct peer *m, const uint8_t key[FLEET_KEY_BYTES])
{
    int i;

    for (i = 0; i < m->node_count; i++) {
        if (memcmp(m->nodes[i].key, key, FLEET_KEY_BYTES) == 0) {
            return &m->nodes[i];
        }
    }
    return NULL;
}

static void apply_nodes(struct peer *m, const cJSON *result)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(result, "nodes");
    const cJSON *item;

    if (!cJSON_IsArray(arr)) {
        return;
    }
    /* A snapshot replaces the list, so a node peerd forgot goes. */
    m->node_count = 0;
    cJSON_ArrayForEach (item, arr) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON *ago = cJSON_GetObjectItemCaseSensitive(item, "heard_ms_ago");
        const cJSON *addr = cJSON_GetObjectItemCaseSensitive(item, "address");
        const cJSON *proved = cJSON_GetObjectItemCaseSensitive(item, "proved");
        struct node *n;
        uint8_t key[FLEET_KEY_BYTES];

        if (m->node_count == NODES_MAX || !cJSON_IsObject(item) || key_of(item, "key", key) != 0 ||
            (m->have_key && memcmp(key, m->key, FLEET_KEY_BYTES) == 0) || node_find(m, key)) {
            continue;
        }
        n = &m->nodes[m->node_count++];
        memset(n, 0, sizeof(*n));
        memcpy(n->key, key, FLEET_KEY_BYTES);
        /* The name was checked by peerd (clean UTF-8, no control character);
         * it is copied, cut to fit, and never interpreted. */
        snprintf(n->name, sizeof(n->name), "%s",
                 cJSON_IsString(name) && name->valuestring ? name->valuestring : "");
        /* Heard this run: when, on our clock. Never heard (a kept peer after
         * a restart): 0, as fleet_link.h says. */
        if (cJSON_IsNumber(ago) && ago->valuedouble >= 0 && ago->valuedouble < 4.0e12) {
            int64_t at = m->now - (int64_t)ago->valuedouble;

            n->heard_ms = at > 0 ? at : 1;
        }
        /* On one network there are no hops: reachable, or no address yet. */
        n->hops = cJSON_IsString(addr) ? 0 : -1;
        n->proved = cJSON_IsTrue(proved);
    }
}

static void apply_status(struct peer *m, const cJSON *result)
{
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(result, "state");
    const cJSON *run = cJSON_GetObjectItemCaseSensitive(result, "run_id");
    const cJSON *ifs = cJSON_GetObjectItemCaseSensitive(result, "interfaces");
    const cJSON *first = cJSON_IsArray(ifs) ? cJSON_GetArrayItem(ifs, 0) : NULL;
    const cJSON *addr = cJSON_GetObjectItemCaseSensitive(first, "address");
    char run_id[sizeof(((struct peer *)0)->run_id)];
    const char *word = cJSON_IsString(state) && state->valuestring ? state->valuestring : "";

    /* Cut as it is stored, so an over-long id is not a new run every time. */
    snprintf(run_id, sizeof(run_id), "%s",
             cJSON_IsString(run) && run->valuestring ? run->valuestring : "");
    m->have_status = 1;
    m->ready = strcmp(word, "ready") == 0;
    snprintf(m->fault, sizeof(m->fault), "%s", m->ready ? "" : word);
    m->socket = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "socket"));
    m->visible = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "visible"));
    m->interfaces = cJSON_IsArray(ifs) ? cJSON_GetArraySize(ifs) : 0;
    /* Bound, and no interface listed (peerd lists them only while its
     * socket is open). Believed the second time running: peerd reads its
     * interfaces a moment after it opens the socket. */
    if (m->ready && m->socket && m->interfaces == 0) {
        if (m->no_net_seen < 2) {
            m->no_net_seen++;
        }
    } else {
        m->no_net_seen = 0;
    }
    m->address[0] = '\0';
    if (cJSON_IsString(addr) && address_ok(addr->valuestring)) {
        snprintf(m->address, sizeof(m->address), "%s", addr->valuestring);
    }
    if (strcmp(run_id, m->run_id) != 0) {
        /* Another run of the service: its ids start again from 1, it has
         * forgotten the lease, and its kept list is to be read again. */
        snprintf(m->run_id, sizeof(m->run_id), "%s", run_id);
        m->cursor = 0;
        m->catching_up = 1;
        m->kept_stale = 1;
    }
    if (m->ready && m->want_visible && !m->visible &&
        m->now - m->last_lease_ms >= STATUS_PERIOD_MS) {
        (void)ask_visible(m);        /* it lapsed, or the service restarted */
    }
    if (m->fd >= 0 && m->ready && m->catching_up) {
        (void)ask_inbox(m);
    }
}

/* One datagram, from an event or the inbox. */
static void take_datagram(struct peer *m, const cJSON *dg)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(dg, "id");
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(dg, "port");
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(dg, "payload_hex");
    const cJSON *at = cJSON_GetObjectItemCaseSensitive(dg, "mono_ms");
    struct rx r;
    int n;
    uint64_t v;

    if (!cJSON_IsNumber(id) || id->valuedouble < 1 || id->valuedouble > 9.0e15 ||
        !cJSON_IsNumber(port) || port->valueint != FLEET_PEER_PORT) {
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
    if (cJSON_IsNumber(at) && at->valuedouble > -4.0e15 && at->valuedouble < 4.0e15 &&
        m->now - (int64_t)at->valuedouble > STALE_MS) {
        m->ignored++;
        return;
    }
    r.n = (size_t)n;
    if (m->rx_count == RX_QUEUE) {
        /* The newest is kept: the protocol retransmits, and the newest
         * carries the most (as on the mesh). */
        m->rx_head = (m->rx_head + 1) % RX_QUEUE;
        m->rx_count--;
        m->dropped++;
    }
    m->rx[(m->rx_head + m->rx_count) % RX_QUEUE] = r;
    m->rx_count++;
    m->taken++;
}

static void apply_inbox(struct peer *m, const cJSON *result)
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

/* peerd's kept list against the match in hand: the opponent is kept, and
 * nobody else is. Fleet is peerd's only client; were there another game, a
 * kept peer would need an owner before this could release what it did not
 * keep (docs/apps/FLEET_MULTIPLAYER.md, "The local network"). */
static void apply_kept(struct peer *m, const cJSON *result)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(result, "kept");
    const cJSON *item;
    int have = 0;
    int count = 0;
    int asked = 0;

    if (!cJSON_IsArray(arr) || !m->match_known) {
        return;
    }
    m->kept_stale = 0;
    cJSON_ArrayForEach (item, arr) {
        uint8_t key[FLEET_KEY_BYTES];

        if (count++ >= KEPT_MAX || key_of(item, "key", key) != 0) {
            continue;
        }
        if (m->have_match && memcmp(key, m->match_key, FLEET_KEY_BYTES) == 0) {
            have = 1;
        } else {
            /* Asked, or could not be written: either way the list is not
             * right yet and is read again. */
            asked++;
            if (ask_key(m, REQ_RELEASE, key) == 0) {
                m->releases++;
            }
        }
    }
    m->kept_count = count;
    if (m->have_match && !have) {
        asked++;
        if (ask_key(m, REQ_KEEP, m->match_key) == 0) {
            m->keeps++;
        }
    }
    if (asked) {
        m->kept_stale = 1;           /* look again once they have been answered */
    } else {
        m->kept_wait_ms = STATUS_PERIOD_MS;
    }
}

/* id is the request this list answers. A list asked for before our probe
 * was registered cannot hold it, and says nothing about it. */
static void apply_probes(struct peer *m, int id, const cJSON *result)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(result, "probes");
    const cJSON *item;

    if (m->probe != FLEET_PEER_PROBE_PENDING || m->probe_id <= 0 || id <= m->probe_listed_from ||
        !cJSON_IsArray(arr)) {
        return;
    }
    cJSON_ArrayForEach (item, arr) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "probe_id");
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(item, "state");

        if (!cJSON_IsNumber(id) || id->valuedouble != (double)m->probe_id ||
            !cJSON_IsString(state)) {
            continue;
        }
        if (strcmp(state->valuestring, "answered") == 0 && key_of(item, "key", m->probe_key) == 0) {
            m->probe = FLEET_PEER_PROBE_ANSWERED;
            m->last_nodes_ms = m->now;
            (void)request(m, REQ_NODES, NULL);   /* it is a player now: show it */
        } else if (strcmp(state->valuestring, "no_answer") == 0) {
            m->probe = FLEET_PEER_PROBE_NO_ANSWER;
        }
        return;
    }
    /* peerd holds four probes and ours is not among them: it was pushed
     * out, or peerd restarted. Either way nobody is going to answer it. */
    m->probe = FLEET_PEER_PROBE_NO_ANSWER;
}

static void kept_failed(struct peer *m)
{
    /* Tried again, and less often each time: a peerd whose store is failing
     * rewrites a file on every keep. */
    m->kept_stale = 1;
    m->kept_wait_ms *= 2;
    if (m->kept_wait_ms > KEPT_PERIOD_MS) {
        m->kept_wait_ms = KEPT_PERIOD_MS;
    }
}

static void on_reply(struct peer *m, enum req what, int id, const cJSON *result, int failed)
{
    if (what == REQ_PROBE && id != m->probe_req) {
        return;                      /* the answer to a probe since replaced */
    }
    if (failed) {
        switch (what) {
        case REQ_SEND:
            /* Refused after the fact (no address for that node yet, not
             * visible, peerd closed). The match has it as sent and retries
             * on its own timer, which is what a lost packet gets. */
            m->send_errors++;
            break;
        case REQ_INBOX:
            m->catching_up = 0;
            break;
        case REQ_PROBE:
            m->probe = FLEET_PEER_PROBE_REFUSED;
            break;
        case REQ_KEEP:
        case REQ_RELEASE:
        case REQ_KEPT:
            kept_failed(m);
            break;
        default:
            break;
        }
        return;
    }
    switch (what) {
    case REQ_IDENTITY:
        if (key_of(result, "key", m->key) == 0) {
            m->have_key = 1;
        }
        break;
    case REQ_STATUS:
        apply_status(m, result);
        break;
    case REQ_VISIBLE:
        m->visible = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "visible"));
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
    case REQ_KEPT:
        apply_kept(m, result);
        break;
    case REQ_PROBE: {
        const cJSON *pid = cJSON_GetObjectItemCaseSensitive(result, "probe_id");

        if (cJSON_IsNumber(pid) && pid->valuedouble >= 1 && pid->valuedouble < 2.0e9) {
            m->probe_id = (long)pid->valuedouble;
            m->probe_listed_from = m->last_id;
            m->last_probes_ms = m->now;
        } else {
            m->probe = FLEET_PEER_PROBE_REFUSED;
        }
        break;
    }
    case REQ_PROBES:
        apply_probes(m, id, result);
        break;
    default:
        break;
    }
}

static void dispatch(struct peer *m, const cJSON *msg)
{
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(msg, "event");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    enum req what;

    if (cJSON_IsString(event) && event->valuestring) {
        /* peer.app carries the datagram as its data. */
        if (strcmp(event->valuestring, "peer.app") == 0 && !m->catching_up) {
            take_datagram(m, cJSON_GetObjectItemCaseSensitive(msg, "data"));
        }
        return;
    }
    if (!cJSON_IsNumber(id)) {
        return;
    }
    if (id->valuedouble < 1 || id->valuedouble > 0x3fffffff) {
        return;
    }
    what = take_pending(m, (int)id->valuedouble);
    if (what == REQ_NONE) {
        return;
    }
    on_reply(m, what, (int)id->valuedouble, cJSON_GetObjectItemCaseSensitive(msg, "result"),
             cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(msg, "error")));
}

static void pump(struct peer *m)
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

static void p_poll(void *ctx, int64_t now)
{
    struct peer *m = ctx;

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
    if (!m->ready) {
        return;                      /* closed by a fault: only its status is asked */
    }
    if (m->want_visible && now - m->last_lease_ms >= LEASE_RENEW_MS && ask_visible(m) != 0) {
        return;
    }
    /* No longer wanted: the lease is ended rather than left to lapse. Not
     * before the opponent of a match in hand is kept - peerd, hidden and
     * keeping nobody, would hear nothing at all. (If the keep keeps failing
     * the lease lapses by itself within 30 s.) */
    if (!m->want_visible && m->hide_due && (!m->have_match || !m->kept_stale)) {
        cJSON *p = cJSON_CreateObject();

        m->hide_due = 0;
        /* Nothing to end where nothing was asked for: a link told not to
         * be seen before it ever connected says nothing about it at all. */
        if (p && m->lease_asked) {
            cJSON_AddBoolToObject(p, "visible", 0);
            m->lease_asked = 0;
            if (request(m, REQ_HIDE, p) != 0) {
                return;
            }
            m->visible = 0;
        } else {
            cJSON_Delete(p);
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
    /* The opponent: once the session has said who (or that there is none),
     * peerd's list is read and put right, and read again now and then. */
    if (m->match_known && (m->kept_stale ? now - m->last_kept_ms >= m->kept_wait_ms || m->last_kept_ms == 0
                                         : now - m->last_kept_ms >= KEPT_PERIOD_MS)) {
        m->last_kept_ms = now ? now : 1;
        if (request(m, REQ_KEPT, NULL) != 0) {
            return;
        }
    }
    if (m->probe == FLEET_PEER_PROBE_PENDING && m->probe_id > 0 &&
        now - m->last_probes_ms >= PROBES_PERIOD_MS) {
        m->last_probes_ms = now;
        (void)request(m, REQ_PROBES, NULL);
    }
}

static enum fleet_link_state p_state(void *ctx)
{
    struct peer *m = ctx;

    if (m->fd < 0) {
        return m->attempts > 0 ? FLEET_LINK_NO_SERVICE : FLEET_LINK_CONNECTING;
    }
    if (!m->have_status) {
        return FLEET_LINK_CONNECTING;
    }
    /* The service is there and nothing can go through it: closed by a
     * fault, which fleet_link_peer_info() names. Having no interface to
     * announce on is not that: a device at a known address is still reached
     * (a kept opponent, a probe), so the link stays up and only the info
     * says so. */
    if (!m->ready) {
        return FLEET_LINK_RADIO_OFF;
    }
    return m->have_key ? FLEET_LINK_UP : FLEET_LINK_CONNECTING;
}

static enum fleet_link_send p_send(void *ctx, const uint8_t to[FLEET_KEY_BYTES], const uint8_t *buf,
                                   size_t n)
{
    struct peer *m = ctx;
    char key[FLEET_KEY_BYTES * 2 + 1];
    char payload[FLEET_PROTO_MAX * 2 + 1];
    cJSON *p;

    if (!buf || n == 0 || n > FLEET_PROTO_MAX) {
        return FLEET_LINK_FAILED;
    }
    if (m->fd < 0) {
        return FLEET_LINK_FAILED;
    }
    if (p_state(m) != FLEET_LINK_UP) {
        return FLEET_LINK_BUSY;      /* nothing goes out; the match holds its attempt */
    }
    p = cJSON_CreateObject();
    if (!p) {
        return FLEET_LINK_BUSY;
    }
    tohex(to, FLEET_KEY_BYTES, key);
    tohex(buf, n, payload);
    cJSON_AddStringToObject(p, "to", key);
    cJSON_AddNumberToObject(p, "port", FLEET_PEER_PORT);
    cJSON_AddStringToObject(p, "payload_hex", payload);
    if (request(m, REQ_SEND, p) != 0) {
        return m->fd < 0 ? FLEET_LINK_FAILED : FLEET_LINK_BUSY;
    }
    m->sends++;
    return FLEET_LINK_SENT;
}

static int p_recv(void *ctx, uint8_t from[FLEET_KEY_BYTES], uint8_t *buf, size_t *n)
{
    struct peer *m = ctx;
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

static int p_self(void *ctx, uint8_t key[FLEET_KEY_BYTES])
{
    struct peer *m = ctx;

    if (!m->have_key) {
        return -1;
    }
    memcpy(key, m->key, FLEET_KEY_BYTES);
    return 0;
}

static int p_peers(void *ctx, struct fleet_link_peer *out, int max)
{
    struct peer *m = ctx;
    int count = 0;
    int i;

    if (max <= 0) {
        return 0;
    }
    for (i = 0; i < m->node_count; i++) {
        const struct node *nd = &m->nodes[i];
        int j;

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

static const char *p_name(void *ctx, const uint8_t key[FLEET_KEY_BYTES])
{
    struct peer *m = ctx;
    const struct node *n = node_find(m, key);

    return n && n->name[0] ? n->name : NULL;
}

/* "Make visible", from a button: on this link the device is visible for as
 * long as multiplayer is open, so the button renews the lease at once. */
static int p_advertise(void *ctx)
{
    struct peer *m = ctx;

    if (m->fd < 0 || p_state(m) != FLEET_LINK_UP || !m->want_visible) {
        return -1;
    }
    return ask_visible(m);
}

static uint32_t p_base(void *ctx)
{
    return ((struct peer *)ctx)->retry_base;
}

static void p_close(void *ctx)
{
    struct peer *m = ctx;

    if (m->fd >= 0) {
        uint8_t waste[1024];
        cJSON *p;
        int rounds;
        int spins;

        /* The request below is the last thing written, and the socket is
         * closed straight after. A pocketipc server answers what it reads in
         * order and drops the connection - with whatever it has not yet
         * handled - at the first answer it cannot deliver; so a request of
         * ours still unanswered when we close takes this one down with it.
         * What peerd owes us is therefore let arrive first: until the socket
         * has been quiet for CLOSE_QUIET_MS, and for at most CLOSE_ROUNDS
         * of that. This is the one wait in the link beyond pocketipc's own,
         * it happens once, as Fleet leaves multiplayer, and it is bounded
         * at 60 ms. If the request is lost all the same, the lease ends
         * this device's visibility within its 30 s. */
        for (rounds = 0, spins = 0; spins < MAX_PENDING; spins++) {
            rounds += m->pending[spins].what != REQ_NONE;
        }
        /* Nothing owed, nothing to wait for. */
        for (rounds = rounds ? 0 : CLOSE_ROUNDS; rounds < CLOSE_ROUNDS; rounds++) {
            struct pollfd pfd = { m->fd, POLLIN, 0 };

            if (poll(&pfd, 1, CLOSE_QUIET_MS) <= 0 || read(m->fd, waste, sizeof(waste)) <= 0) {
                break;
            }
        }
        p = cJSON_CreateObject();
        /* Fleet is leaving multiplayer: off the network at once, rather
         * than when the lease would have lapsed (which is what happens
         * anyway if this does not get through). A match in hand is not
         * affected - peerd goes on serving the kept opponent. */
        if (p && m->lease_asked) {
            cJSON_AddBoolToObject(p, "visible", 0);
            (void)request(m, REQ_HIDE, p);
        } else {
            cJSON_Delete(p);         /* no lease of ours to end */
        }
        if (m->fd >= 0 && m->subscribed) {
            (void)request(m, REQ_UNSUBSCRIBE, NULL);
        }
        if (m->fd >= 0) {
            close(m->fd);
        }
    }
    pocketipc_reader_free(&m->reader);
    free(m);
}

static const struct fleet_link_profile *p_profile(void *ctx)
{
    (void)ctx;
    return &fleet_profile_lan;
}

/* The opponent of the match in hand, or NULL when there is none. Said by the
 * session on every poll; acted on when it changes. */
static void p_match_peer(void *ctx, const uint8_t *key)
{
    struct peer *m = ctx;
    int have = key != NULL;

    if (m->match_known && m->have_match == have &&
        (!have || memcmp(m->match_key, key, FLEET_KEY_BYTES) == 0)) {
        return;
    }
    m->match_known = 1;
    m->have_match = have;
    if (have) {
        memcpy(m->match_key, key, FLEET_KEY_BYTES);
    } else {
        memset(m->match_key, 0, sizeof(m->match_key));
    }
    m->kept_stale = 1;
    m->kept_wait_ms = STATUS_PERIOD_MS;
    m->last_kept_ms = 0;             /* at the next poll, not at the next period */
}

static const struct fleet_link_ops OPS = {
    p_poll, p_send, p_recv, p_self, p_state, p_peers, p_name, p_advertise, p_base, p_close, p_profile,
    p_match_peer,
};

int fleet_link_peer_present(const char *service)
{
    char path[256];
    struct stat sb;

    return pocketipc_socket_path(service && *service ? service : FLEET_PEER_SERVICE, path,
                                 sizeof(path)) == 0 &&
           stat(path, &sb) == 0 && S_ISSOCK(sb.st_mode);
}

struct fleet_link *fleet_link_peer_open(const char *service)
{
    struct peer *m = calloc(1, sizeof(*m));

    if (!m) {
        return NULL;
    }
    snprintf(m->service, sizeof(m->service), "%s", service && *service ? service : FLEET_PEER_SERVICE);
    m->fd = -1;
    m->next_id = 1;
    m->backoff_ms = BACKOFF_MIN_MS;
    m->kept_count = -1;
    m->kept_stale = 1;
    m->kept_wait_ms = STATUS_PERIOD_MS;
    m->want_visible = 1;
    pocketipc_reader_init(&m->reader);
    m->link.ops = &OPS;
    m->link.ctx = m;
    return &m->link;
}

/* ---- what only this link has -------------------------------------------------- */

static struct peer *peer_of(const struct fleet_link *link)
{
    return link && link->ops == &OPS ? link->ctx : NULL;
}

void fleet_link_peer_set_visible(struct fleet_link *link, int visible)
{
    struct peer *m = peer_of(link);

    visible = visible != 0;
    if (!m || m->want_visible == visible) {
        return;
    }
    m->want_visible = visible;
    if (!visible) {
        m->hide_due = 1;             /* written at the next poll (p_poll) */
    } else {
        m->hide_due = 0;
        if (m->fd >= 0 && m->ready) {
            (void)ask_visible(m);    /* at once: the lobby has just opened */
        }
    }
}

int fleet_link_peer_info(const struct fleet_link *link, struct fleet_link_peer_info *out)
{
    const struct peer *m = peer_of(link);

    if (!m || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->kept = m->kept_count;
    out->visible = m->visible;
    snprintf(out->address, sizeof(out->address), "%s", m->address);
    if (m->fd < 0 || !m->have_status) {
        out->trouble = m->fd < 0 && m->attempts > 0 ? FLEET_PEER_NO_SERVICE : FLEET_PEER_OK;
    } else if (!m->ready) {
        out->trouble = FLEET_PEER_FAULT;
        snprintf(out->fault, sizeof(out->fault), "%s", m->fault);
    } else if (m->no_net_seen >= 2) {
        out->trouble = FLEET_PEER_NO_NETWORK;
    }
    return 0;
}

int fleet_link_peer_probe(struct fleet_link *link, const char *address)
{
    struct peer *m = peer_of(link);
    cJSON *p;

    if (!m) {
        return -1;
    }
    if (!address_ok(address)) {
        m->probe = FLEET_PEER_PROBE_REFUSED;
        return -1;
    }
    if (m->fd < 0 || p_state(m) != FLEET_LINK_UP || !(p = cJSON_CreateObject())) {
        m->probe = FLEET_PEER_PROBE_REFUSED;
        return -1;
    }
    cJSON_AddStringToObject(p, "address", address);
    m->probe = FLEET_PEER_PROBE_PENDING;
    m->probe_id = 0;
    memset(m->probe_key, 0, sizeof(m->probe_key));
    if (request(m, REQ_PROBE, p) != 0) {
        m->probe = FLEET_PEER_PROBE_REFUSED;
        return -1;
    }
    m->probe_req = m->last_id;
    return 0;
}

int fleet_link_peer_proved(const struct fleet_link *link, const uint8_t key[FLEET_KEY_BYTES])
{
    struct peer *m = peer_of(link);
    const struct node *n = m && key ? node_find(m, key) : NULL;

    return n ? n->proved : -1;
}

enum fleet_link_peer_probe_state fleet_link_peer_probe_state(const struct fleet_link *link)
{
    const struct peer *m = peer_of(link);

    return m ? m->probe : FLEET_PEER_PROBE_NONE;
}

int fleet_link_peer_probe_key(const struct fleet_link *link, uint8_t key[FLEET_KEY_BYTES])
{
    const struct peer *m = peer_of(link);

    if (!m || !key || m->probe != FLEET_PEER_PROBE_ANSWERED) {
        return -1;
    }
    memcpy(key, m->probe_key, FLEET_KEY_BYTES);
    return 0;
}

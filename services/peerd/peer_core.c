/*
 * peerd's engine (peer_core.h, docs/decisions/ADR-017).
 *
 * The order of the checks on a received frame is the security of this file:
 * everything that is cheap and needs no key comes first, a public-key
 * operation is only ever spent within a budget, and no state changes before
 * a frame's tag or signature has verified.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "peer_core.h"

#include "peer_replay.h"

#include <stdlib.h>
#include <string.h>

#define NONKEPT_MAX (PCORE_NODES - PCORE_KEPT)

/* Verifications per second: a budget each for keys we know and keys we do
 * not, so a flood of invented keys cannot starve a genuine peer's beacons. */
#define VERIFY_KNOWN_PER_S 200
#define VERIFY_UNKNOWN_PER_S 20
#define VERIFY_PER_NODE_PER_S 50        /* and one key's frames cannot spend what the others need (S7) */
#define REPLY_PER_S 10                  /* all replies together: a source address can be forged */
#define REPLY_KEPT_PER_S 10             /* kept peers have their own, so strangers cannot spend it */
#define REPLY_PER_NODE_MS 1000

struct bucket {
    int64_t milli;      /* tokens * 1000 */
    int64_t last;
    int per_s;
};

struct node {
    uint8_t used;
    int8_t kept;                        /* index into kept[], or -1 */
    uint8_t have_sess;
    uint8_t proved;
    uint8_t table_on;
    uint8_t lookup_on;
    uint8_t floor_loaded;               /* a kept peer whose floor was read from disk at start */
    uint8_t key[PC_KEY];
    struct pc_session sess;
    struct pr_window win;
    uint32_t addr;
    uint16_t port;
    int64_t heard;                      /* -1: not this run */
    int64_t table_heard;
    int64_t last_reply;
    int64_t lookup_next;
    struct pw_table table;
    uint8_t lookup_challenge[PW_CHALLENGE];
    char name[PW_NAME_MAX + 1];
    struct bucket verify;               /* signature checks spent on this key */
};

struct slot {
    uint8_t used;
    uint8_t held;                       /* waiting for its floor to be on disk */
    uint8_t from_kept;
    uint64_t order;                     /* arrival order, for "the oldest" */
    uint64_t hold_epoch;                /* the frame it came in: delivered once the */
    uint64_t hold_seq;                  /* sender's floor ON DISK is at or above this */
    struct pcore_dgram d;
};

struct probe {
    uint32_t id;
    uint8_t state;
    uint8_t tries;
    uint32_t addr;
    uint16_t port;
    int64_t next;
    uint8_t challenge[PW_CHALLENGE];
    uint8_t key[PC_KEY];
};

struct pcore {
    const struct pc_identity *id;
    struct pcore_ops ops;
    void *user;
    uint64_t epoch;
    uint64_t seq;                       /* the last sequence used */
    uint16_t udp_port;
    char name[PW_NAME_MAX + 1];

    struct pcore_iface ifs[PCORE_IFACES];
    int n_ifs;

    int64_t lease_until;
    int64_t next_beacon;
    uint8_t announcing;
    struct pw_table announce;

    struct node nodes[PCORE_NODES];
    struct ps_kept kept[PCORE_KEPT];
    uint8_t kept_used[PCORE_KEPT];
    /* Each kept peer's replay floor as it is on disk, which trails kept[]
     * between a datagram's arrival and the write. What may be delivered is
     * decided against this, never against the copy in memory (S1). */
    uint64_t disk_epoch[PCORE_KEPT];
    uint64_t disk_mark[PCORE_KEPT];
    uint8_t kept_dirty;
    int64_t kept_written;               /* when the list was last written */

    struct slot inbox[PCORE_INBOX];
    uint64_t next_id;
    uint64_t next_order;

    struct probe probes[PCORE_PROBES];
    uint32_t next_probe_id;

    struct bucket verify_known;
    struct bucket verify_unknown;
    struct bucket replies;
    struct bucket replies_kept;

    uint64_t count[PCORE_C_COUNT];
};

/* ---- small things ------------------------------------------------------------- */

static void bump(struct pcore *c, enum pcore_count which)
{
    c->count[which]++;
}

static void bucket_init(struct bucket *b, int per_s)
{
    b->per_s = per_s;
    b->milli = (int64_t)per_s * 1000;
    b->last = 0;
}

static int bucket_take(struct bucket *b, int64_t now)
{
    int64_t cap = (int64_t)b->per_s * 1000;

    if (now > b->last) {
        int64_t add = (now - b->last) * b->per_s;

        b->milli = add >= cap - b->milli ? cap : b->milli + add;
        b->last = now;
    }
    if (b->milli < 1000) {
        return 0;
    }
    b->milli -= 1000;
    return 1;
}

/* An address a node can be sent to: not zero, not broadcast, not multicast
 * or reserved, not one of ours. */
static int endpoint_ok(const struct pcore *c, uint32_t addr)
{
    int i;

    if (addr == 0 || addr == 0xFFFFFFFFu || (addr >> 28) >= 0xE) {
        return 0;
    }
    for (i = 0; i < c->n_ifs; i++) {
        if (addr == c->ifs[i].addr || addr == c->ifs[i].bcast) {
            return 0;
        }
    }
    return 1;
}

static struct node *node_find(struct pcore *c, const uint8_t key[PC_KEY])
{
    int i;

    for (i = 0; i < PCORE_NODES; i++) {
        if (c->nodes[i].used && memcmp(c->nodes[i].key, key, PC_KEY) == 0) {
            return &c->nodes[i];
        }
    }
    return NULL;
}

static void node_clear(struct node *n)
{
    pc_session_wipe(&n->sess);
    memset(n, 0, sizeof(*n));
}

/* A slot for a new node. Kept peers have PCORE_KEPT slots to themselves:
 * the rest are replaced least-recently-heard first, and a kept peer is never
 * the one replaced. */
static struct node *node_new(struct pcore *c, const uint8_t key[PC_KEY], int for_kept)
{
    struct node *free_slot = NULL;
    struct node *oldest = NULL;
    int nonkept = 0;
    int i;

    for (i = 0; i < PCORE_NODES; i++) {
        struct node *n = &c->nodes[i];

        if (!n->used) {
            if (!free_slot) {
                free_slot = n;
            }
            continue;
        }
        if (n->kept < 0) {
            nonkept++;
            if (!oldest || n->heard < oldest->heard) {
                oldest = n;
            }
        }
    }
    if (!for_kept && nonkept >= NONKEPT_MAX) {
        free_slot = NULL;               /* the reserved slots are not for strangers */
    }
    if (!free_slot) {
        if (!oldest) {
            return NULL;
        }
        node_clear(oldest);
        bump(c, PCORE_C_EVICTED_NODES);
        free_slot = oldest;
    }
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = 1;
    free_slot->kept = -1;
    free_slot->heard = -1;
    free_slot->table_heard = -1;
    free_slot->last_reply = -REPLY_PER_NODE_MS;
    bucket_init(&free_slot->verify, VERIFY_PER_NODE_PER_S);
    memcpy(free_slot->key, key, PC_KEY);
    return free_slot;
}

static int node_session(struct pcore *c, struct node *n)
{
    if (!n->have_sess) {
        if (pc_session(c->id, n->key, &n->sess) != 0) {
            return -1;
        }
        n->have_sess = 1;
    }
    return 0;
}

/* The next sequence, or 0 when this run has none left. */
static uint64_t next_seq(struct pcore *c)
{
    if (c->seq == UINT64_MAX || c->epoch == 0) {
        return 0;
    }
    return ++c->seq;
}

static int transmit(struct pcore *c, uint32_t dst, uint16_t port, const uint8_t *buf, size_t n,
                    int bcast)
{
    if (!c->ops.send || c->ops.send(c->user, dst, port, buf, n, bcast) != 0) {
        bump(c, PCORE_C_TX_FAILED);
        return -1;
    }
    bump(c, PCORE_C_TX);
    return 0;
}

/* ---- kept peers ---------------------------------------------------------------- */

static int kept_count(const struct pcore *c)
{
    int i;
    int n = 0;

    for (i = 0; i < PCORE_KEPT; i++) {
        n += c->kept_used[i];
    }
    return n;
}

static int kept_pack(const struct pcore *c, struct ps_kept *out)
{
    int i;
    int n = 0;

    for (i = 0; i < PCORE_KEPT; i++) {
        if (c->kept_used[i]) {
            out[n++] = c->kept[i];
        }
    }
    return n;
}

static int pair_gt(uint64_t e1, uint64_t s1, uint64_t e2, uint64_t s2)
{
    return e1 > e2 || (e1 == e2 && s1 > s2);
}

static void flush_held(struct pcore *c);

static int kept_write(struct pcore *c, int64_t now)
{
    struct ps_kept recs[PCORE_KEPT];
    int n = kept_pack(c, recs);
    int i;

    if (!c->ops.kept_save || c->ops.kept_save(c->user, recs, n) != 0) {
        bump(c, PCORE_C_KEPT_WRITE_FAILED);
        return -1;
    }
    bump(c, PCORE_C_KEPT_WRITES);
    c->kept_dirty = 0;
    c->kept_written = now;
    for (i = 0; i < PCORE_KEPT; i++) {
        c->disk_epoch[i] = c->kept_used[i] ? c->kept[i].epoch : 0;
        c->disk_mark[i] = c->kept_used[i] ? c->kept[i].mark : 0;
    }
    /* Whoever caused the write, every datagram it covers may go now (S2). */
    flush_held(c);
    return 0;
}

/* ---- the inbox ----------------------------------------------------------------- */

static void slot_deliver(struct pcore *c, struct slot *s)
{
    s->held = 0;
    s->d.id = ++c->next_id;
    if (c->ops.delivered) {
        c->ops.delivered(c->user, &s->d);
    }
}

/* Deliver every held datagram whose sender's floor on disk has reached it -
 * or whose sender is no longer kept, so there is nothing to wait for - in
 * the order they arrived. */
static void flush_held(struct pcore *c)
{
    for (;;) {
        struct slot *next = NULL;
        int i;

        for (i = 0; i < PCORE_INBOX; i++) {
            struct slot *s = &c->inbox[i];
            struct node *n;

            if (!s->used || !s->held) {
                continue;
            }
            n = node_find(c, s->d.from);
            if (n && n->kept >= 0 &&
                pair_gt(s->hold_epoch, s->hold_seq, c->disk_epoch[n->kept], c->disk_mark[n->kept])) {
                continue;               /* its floor is not on disk yet */
            }
            if (!next || s->order < next->order) {
                next = s;
            }
        }
        if (!next) {
            return;
        }
        slot_deliver(c, next);
    }
}

/* Room for a datagram from `from`. At most PCORE_INBOX_PER_SENDER of one
 * sender's (its own oldest goes); a kept peer's is never pushed out by
 * another sender's; and a kept peer's arriving at a full inbox pushes out
 * the oldest that is not a kept peer's. */
static struct slot *inbox_room(struct pcore *c, const uint8_t from[PC_KEY], int from_kept)
{
    struct slot *free_slot = NULL;
    struct slot *own_oldest = NULL;
    struct slot *nonkept_oldest = NULL;
    int own = 0;
    int i;

    for (i = 0; i < PCORE_INBOX; i++) {
        struct slot *s = &c->inbox[i];

        if (!s->used) {
            if (!free_slot) {
                free_slot = s;
            }
            continue;
        }
        if (memcmp(s->d.from, from, PC_KEY) == 0) {
            own++;
            if (!own_oldest || s->order < own_oldest->order) {
                own_oldest = s;
            }
        }
        if (!s->from_kept && (!nonkept_oldest || s->order < nonkept_oldest->order)) {
            nonkept_oldest = s;
        }
    }
    if (own >= PCORE_INBOX_PER_SENDER) {
        return own_oldest;
    }
    if (free_slot) {
        return free_slot;
    }
    (void)from_kept;
    /* Full. Only a datagram that is not a kept peer's may go - whoever is
     * arriving. With none of those there is no room (and a kept peer always
     * has room: PCORE_KEPT senders fill the inbox only at their own caps). */
    return nonkept_oldest;
}

/* ---- sending -------------------------------------------------------------------- */

static int send_signed(struct pcore *c, uint8_t type, uint32_t dst, uint16_t port, uint32_t stated,
                       const uint8_t *recipient, const uint8_t *challenge, const uint8_t *target,
                       int bcast)
{
    uint8_t buf[PW_SIGNED_SIZE];
    struct pw_signed f;

    memset(&f, 0, sizeof(f));
    f.h.type = type;
    memcpy(f.h.sender, c->id->pk, PC_KEY);
    if (recipient) {
        memcpy(f.h.recipient, recipient, PW_PREFIX);
    }
    f.h.epoch = c->epoch;
    f.addr = stated;
    f.port = c->udp_port;
    f.name_len = (uint8_t)strlen(c->name);
    memcpy(f.name, c->name, f.name_len);
    if (type == PW_BEACON && c->announcing) {
        f.flags = PW_FLAG_TABLE;
        f.table = c->announce;
    }
    if (challenge) {
        memcpy(f.challenge, challenge, PW_CHALLENGE);
    }
    if (target) {
        memcpy(f.target, target, PC_KEY);
    }
    f.h.seq = next_seq(c);
    if (f.h.seq == 0 || pw_build_signed(&f, buf) != PW_OK ||
        pc_sign(c->id, buf, PW_SIGNED_BODY, buf + PW_SIGNED_BODY) != 0) {
        bump(c, PCORE_C_TX_FAILED);
        return -1;
    }
    return transmit(c, dst, port, buf, sizeof(buf), bcast);
}

static uint32_t source_for(struct pcore *c, uint32_t dst)
{
    return c->ops.source_for ? c->ops.source_for(c->user, dst) : 0;
}

/* A reply to a node's request: within the global limit and that node's own. */
static void send_reply(struct pcore *c, struct node *n, uint8_t type, uint32_t dst, uint16_t port,
                       const uint8_t *challenge, int64_t now)
{
    uint32_t stated = source_for(c, dst);

    if (stated == 0 || now - n->last_reply < REPLY_PER_NODE_MS ||
        !bucket_take(n->kept >= 0 ? &c->replies_kept : &c->replies, now)) {
        bump(c, PCORE_C_REPLY_LIMITED);
        return;
    }
    n->last_reply = now;
    send_signed(c, type, dst, port, stated, n->key, challenge, NULL, 0);
}

static void send_beacons(struct pcore *c)
{
    int i;

    for (i = 0; i < c->n_ifs; i++) {
        if (c->ifs[i].addr && c->ifs[i].bcast) {
            send_signed(c, PW_BEACON, c->ifs[i].bcast, c->udp_port, c->ifs[i].addr, NULL, NULL,
                        NULL, 1);
        }
    }
}

/* Ask where a kept peer is: its last address, and every interface. Only
 * that peer answers, and only with our challenge in its signature. */
static void lookup_start(struct pcore *c, struct node *n, int64_t now)
{
    int i;

    if (n->lookup_on && now < n->lookup_next) {
        return;
    }
    pc_random(n->lookup_challenge, PW_CHALLENGE);
    n->lookup_on = 1;
    n->lookup_next = now + PCORE_LOOKUP_INTERVAL_MS;
    if (n->addr) {
        uint32_t stated = source_for(c, n->addr);

        if (stated) {
            send_signed(c, PW_LOOKUP, n->addr, n->port ? n->port : c->udp_port, stated, NULL,
                        n->lookup_challenge, n->key, 0);
        }
    }
    for (i = 0; i < c->n_ifs; i++) {
        if (c->ifs[i].addr && c->ifs[i].bcast) {
            send_signed(c, PW_LOOKUP, c->ifs[i].bcast, c->udp_port, c->ifs[i].addr, NULL,
                        n->lookup_challenge, n->key, 1);
        }
    }
}

/* ---- receiving: DATA ------------------------------------------------------------ */

static void endpoint_set(struct pcore *c, struct node *n, uint32_t addr, uint16_t port)
{
    if (!endpoint_ok(c, addr) || port == 0 || (n->addr == addr && n->port == port)) {
        return;
    }
    n->addr = addr;
    n->port = port;
    if (n->kept >= 0) {
        c->kept[n->kept].addr = addr;
        c->kept[n->kept].port = port;
        c->kept_dirty = 1;              /* written under the same cap; delivery does not wait for it */
    }
}

static void input_data(struct pcore *c, const struct pw_header *h, uint32_t src, uint16_t sport,
                       const uint8_t *buf, size_t n, int64_t now)
{
    uint8_t pt[1 + PW_PAYLOAD_MAX];
    size_t ctlen = n - PW_HEADER - PW_NONCE;
    size_t plen = ctlen - PW_TAG;
    struct node *node;
    struct slot *s;
    enum pr_verdict v;
    int hold = 0;

    if (memcmp(h->recipient, c->id->pk, PW_PREFIX) != 0) {
        bump(c, PCORE_C_OTHER);
        return;
    }
    node = node_find(c, h->sender);
    if (!node) {
        bump(c, PCORE_C_UNKNOWN);       /* no public-key operation was spent on it */
        return;
    }
    if (node->kept < 0 && !pcore_visible(c, now)) {
        bump(c, PCORE_C_NOT_VISIBLE);
        return;
    }
    v = pr_check(&node->win, h->epoch, h->seq);
    if (v < 0) {
        bump(c, PCORE_C_REPLAY);
        return;
    }
    if (node_session(c, node) != 0) {
        bump(c, PCORE_C_BAD_KEY);
        return;
    }
    if (pc_open(node->sess.rx, buf, PW_HEADER, buf + PW_HEADER, buf + PW_HEADER + PW_NONCE, ctlen,
                pt) != 0) {
        bump(c, PCORE_C_AUTH);
        return;
    }
    /* Authentic from here on. */
    pr_commit(&node->win, h->epoch, h->seq);
    if (plen < 2 || pt[0] < PW_PORT_MIN || pt[0] > PW_PORT_MAX) {
        bump(c, PCORE_C_MALFORMED);
        pc_wipe(pt, sizeof(pt));
        return;
    }
    node->heard = now;
    node->lookup_on = 0;
    if (v == PR_NEWER) {
        endpoint_set(c, node, src, sport);
    }
    if (node->kept >= 0) {
        struct ps_kept *k = &c->kept[node->kept];

        /* A kept peer whose floor was on disk when this run began: this
         * datagram is above everything recorded from it before, so it is
         * not a recording - it is live. A floor made in this run proves
         * nothing (it may itself have been set by a recording), and for a
         * node that is not kept a sealed datagram may be one replayed into
         * a receiver that has forgotten it (ADR-017 section 6). */
        if (node->floor_loaded) {
            node->proved = 1;
        }
        /* The replay floor: the newest DATA frame, epoch first. */
        if (pair_gt(h->epoch, h->seq, k->epoch, k->mark)) {
            k->epoch = h->epoch;
            k->mark = h->seq;
            c->kept_dirty = 1;
        }
        /* It reaches the inbox only once a floor at or above it is ON DISK,
         * so an opponent's move can never be replayed into a restarted
         * device - not this one, and not an older one accepted beside it. */
        hold = pair_gt(h->epoch, h->seq, c->disk_epoch[node->kept], c->disk_mark[node->kept]);
    }
    s = inbox_room(c, node->key, node->kept >= 0);
    if (!s) {
        bump(c, PCORE_C_INBOX_FULL);
        pc_wipe(pt, sizeof(pt));
        return;
    }
    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->from_kept = node->kept >= 0;
    s->order = ++c->next_order;
    s->hold_epoch = h->epoch;
    s->hold_seq = h->seq;
    memcpy(s->d.from, node->key, PC_KEY);
    s->d.port = pt[0];
    s->d.len = (uint16_t)(plen - 1);
    memcpy(s->d.data, pt + 1, plen - 1);
    s->d.mono_ms = now;
    pc_wipe(pt, sizeof(pt));
    bump(c, PCORE_C_ACCEPTED_DATA);
    if (!hold) {
        slot_deliver(c, s);
        return;
    }
    s->held = 1;
    if (now - c->kept_written >= PCORE_KEPT_WRITE_MS) {
        kept_write(c, now);             /* on success it delivers this and all it covers */
    }
}

/* ---- receiving: signed frames ----------------------------------------------------- */

static struct probe *probe_match(struct pcore *c, const struct pw_signed *f, uint32_t src)
{
    int i;

    for (i = 0; i < PCORE_PROBES; i++) {
        struct probe *p = &c->probes[i];

        if (p->state == PCORE_PROBE_PENDING && p->addr == src && p->port == f->port &&
            memcmp(p->challenge, f->challenge, PW_CHALLENGE) == 0) {
            return p;
        }
    }
    return NULL;
}

static void input_signed(struct pcore *c, uint32_t src, const uint8_t *buf, size_t n, int64_t now)
{
    struct pw_signed f;
    struct node *node;
    enum pr_verdict v = PR_NEWER;
    int visible = pcore_visible(c, now);
    int stated_ok;
    int reply_type;
    int fresh;

    if (pw_parse_signed(buf, n, &f) != PW_OK) {
        bump(c, PCORE_C_MALFORMED);
        return;
    }
    reply_type = f.h.type == PW_PROBE_REPLY || f.h.type == PW_LOOKUP_REPLY;
    node = node_find(c, f.h.sender);
    /* Not visible: a stranger is not heard at all, and nobody but a kept
     * peer is answered. */
    if (!visible && !(node && node->kept >= 0)) {
        bump(c, PCORE_C_NOT_VISIBLE);
        return;
    }
    if ((reply_type && memcmp(f.h.recipient, c->id->pk, PW_PREFIX) != 0) ||
        (f.h.type == PW_LOOKUP && memcmp(f.target, c->id->pk, PC_KEY) != 0)) {
        bump(c, PCORE_C_OTHER);
        return;
    }
    stated_ok = f.addr == src && endpoint_ok(c, src);
    if (!node && !stated_ok) {
        bump(c, PCORE_C_STATED);
        return;
    }
    if (node) {
        v = pr_check(&node->win, f.h.epoch, f.h.seq);
        if (v < 0) {
            bump(c, PCORE_C_REPLAY);    /* before any signature is checked */
            return;
        }
    }
    /* A known key spends its own budget first, so frames forged in one
     * key's name (keys travel in clear) cannot spend what the others need. */
    if (node ? !bucket_take(&node->verify, now) || !bucket_take(&c->verify_known, now)
             : !bucket_take(&c->verify_unknown, now)) {
        bump(c, PCORE_C_BUDGET);
        return;
    }
    if (!node && !pc_peer_ok(c->id, f.h.sender)) {
        bump(c, PCORE_C_BAD_KEY);
        return;
    }
    if (pc_verify(f.h.sender, buf, PW_SIGNED_BODY, buf + PW_SIGNED_BODY) != 0) {
        bump(c, PCORE_C_AUTH);
        return;
    }
    /* Authentic from here on. */
    if (!node) {
        node = node_new(c, f.h.sender, 0);
        if (!node) {
            bump(c, PCORE_C_BAD_KEY);
            return;
        }
    }
    pr_commit(&node->win, f.h.epoch, f.h.seq);
    bump(c, PCORE_C_ACCEPTED_SIGNED);
    /* Only the newest frame, from the address it states, changes what this
     * node is shown as or where it is: an older one that was missed, or one
     * relayed from elsewhere, is recorded as seen and changes nothing. */
    fresh = stated_ok && v == PR_NEWER;
    if (fresh) {
        node->heard = now;
        memcpy(node->name, f.name, sizeof(node->name));
        endpoint_set(c, node, src, f.port);
    }
    switch (f.h.type) {
    case PW_BEACON:
        if (fresh) {
            node->table_on = (f.flags & PW_FLAG_TABLE) != 0;
            node->table = f.table;
            node->table_heard = now;
        }
        break;
    case PW_PROBE:
        if (stated_ok) {
            send_reply(c, node, PW_PROBE_REPLY, src, f.port, f.challenge, now);
        }
        break;
    case PW_PROBE_REPLY: {
        struct probe *p = probe_match(c, &f, src);

        if (p && stated_ok) {
            /* Our own random bytes, signed by this key, from the address we
             * asked: the node is live, here, now - whatever its sequence. */
            p->state = PCORE_PROBE_OK;
            memcpy(p->key, node->key, PC_KEY);
            node->proved = 1;
            node->heard = now;
            memcpy(node->name, f.name, sizeof(node->name));
            endpoint_set(c, node, src, f.port);
        }
        break;
    }
    case PW_LOOKUP:
        if (stated_ok) {
            send_reply(c, node, PW_LOOKUP_REPLY, src, f.port, f.challenge, now);
        }
        break;
    case PW_LOOKUP_REPLY:
        if (node->lookup_on && stated_ok &&
            memcmp(node->lookup_challenge, f.challenge, PW_CHALLENGE) == 0) {
            node->lookup_on = 0;
            node->proved = 1;
            node->heard = now;
            memcpy(node->name, f.name, sizeof(node->name));
            endpoint_set(c, node, src, f.port);
        }
        break;
    default:
        break;
    }
}

void pcore_input(struct pcore *c, uint32_t src, uint16_t src_port, const uint8_t *buf, size_t n,
                 int64_t now)
{
    struct pw_header h;

    if (!c || !buf) {
        return;
    }
    bump(c, PCORE_C_RX);
    if (pw_parse_header(buf, n, &h) != PW_OK) {
        bump(c, PCORE_C_MALFORMED);
        return;
    }
    /* Our own broadcasts come back to us; a cloned device has our key. */
    if (memcmp(h.sender, c->id->pk, PC_KEY) == 0) {
        bump(c, PCORE_C_OWN);
        return;
    }
    if (h.type == PW_DATA) {
        input_data(c, &h, src, src_port, buf, n, now);
    } else {
        input_signed(c, src, buf, n, now);
    }
}

/* ---- time ------------------------------------------------------------------------- */

void pcore_tick(struct pcore *c, int64_t now)
{
    int i;

    if (!c) {
        return;
    }
    /* Held datagrams: one write of the newest floors covers all of them
     * (kept_write delivers what it covers). The flush is unconditional, so
     * nothing a write elsewhere covered is ever left waiting. */
    if (c->kept_dirty && now - c->kept_written >= PCORE_KEPT_WRITE_MS) {
        kept_write(c, now);
    }
    flush_held(c);
    if (pcore_visible(c, now) && now >= c->next_beacon) {
        c->next_beacon = now + PCORE_BEACON_MS;
        send_beacons(c);
    }
    for (i = 0; i < PCORE_PROBES; i++) {
        struct probe *p = &c->probes[i];

        if (p->state != PCORE_PROBE_PENDING) {
            continue;
        }
        if (!pcore_visible(c, now)) {
            p->state = PCORE_PROBE_FAILED;      /* a probe does not outlive the lease */
            continue;
        }
        if (now < p->next) {
            continue;
        }
        if (p->tries >= PCORE_PROBE_TRIES) {
            p->state = PCORE_PROBE_FAILED;
            continue;
        }
        p->tries++;
        p->next = now + PCORE_PROBE_INTERVAL_MS;
        {
            uint32_t stated = source_for(c, p->addr);

            if (stated) {
                send_signed(c, PW_PROBE, p->addr, p->port, stated, NULL, p->challenge, NULL, 0);
            }
        }
    }
}

int pcore_wants_socket(const struct pcore *c, int64_t now)
{
    int i;

    if (!c) {
        return 0;
    }
    if (pcore_visible(c, now) || kept_count(c) > 0) {
        return 1;
    }
    for (i = 0; i < PCORE_PROBES; i++) {
        if (c->probes[i].state == PCORE_PROBE_PENDING) {
            return 1;
        }
    }
    return 0;
}

/* ---- what a client drives ----------------------------------------------------------- */

void pcore_set_visible(struct pcore *c, int on, int64_t now)
{
    if (!c) {
        return;
    }
    if (on) {
        if (!pcore_visible(c, now)) {
            c->next_beacon = now;       /* be heard at once */
        }
        c->lease_until = now + PCORE_LEASE_MS;
    } else {
        c->lease_until = 0;
    }
}

int pcore_visible(const struct pcore *c, int64_t now)
{
    return c && c->lease_until != 0 && now < c->lease_until;
}

int pcore_announce(struct pcore *c, uint8_t port, uint8_t proto_min, uint8_t proto_max,
                   uint8_t seats, uint32_t *tag)
{
    if (!c || port < PW_PORT_MIN || port > PW_PORT_MAX || proto_min > proto_max || seats == 0) {
        return PCORE_E_PARAM;
    }
    c->announce.port = port;
    c->announce.proto_min = proto_min;
    c->announce.proto_max = proto_max;
    c->announce.seats = seats;
    pc_random(&c->announce.tag, sizeof(c->announce.tag));
    c->announcing = 1;
    if (tag) {
        *tag = c->announce.tag;
    }
    return PCORE_OK;
}

void pcore_withdraw(struct pcore *c)
{
    if (c) {
        c->announcing = 0;
        memset(&c->announce, 0, sizeof(c->announce));
    }
}

int pcore_probe(struct pcore *c, uint32_t addr, uint16_t port, int64_t now)
{
    struct probe *slot = NULL;
    int i;

    if (!c || port == 0 || !endpoint_ok(c, addr)) {
        return PCORE_E_PARAM;
    }
    if (!pcore_visible(c, now)) {
        return PCORE_E_CLOSED;
    }
    for (i = 0; i < PCORE_PROBES; i++) {
        if (c->probes[i].state != PCORE_PROBE_PENDING &&
            (!slot || c->probes[i].id < slot->id)) {
            slot = &c->probes[i];       /* a finished one, the oldest first */
        }
    }
    if (!slot) {
        return PCORE_E_FULL;
    }
    memset(slot, 0, sizeof(*slot));
    slot->id = ++c->next_probe_id;
    slot->state = PCORE_PROBE_PENDING;
    slot->addr = addr;
    slot->port = port;
    slot->next = now;
    pc_random(slot->challenge, PW_CHALLENGE);
    return (int)slot->id;
}

int pcore_probes(const struct pcore *c, struct pcore_probe_info *out, int max)
{
    int n = 0;
    int i;

    for (i = 0; c && i < PCORE_PROBES && n < max; i++) {
        const struct probe *p = &c->probes[i];

        if (p->state == PCORE_PROBE_NONE) {
            continue;
        }
        out[n].id = p->id;
        out[n].addr = p->addr;
        out[n].port = p->port;
        out[n].state = (enum pcore_probe_state)p->state;
        memcpy(out[n].key, p->key, PC_KEY);
        n++;
    }
    return n;
}

int pcore_keep(struct pcore *c, const uint8_t key[PC_KEY])
{
    struct node *n;
    int slot = -1;
    int made = 0;
    int i;

    if (!c || !key || !pc_peer_ok(c->id, key)) {
        return PCORE_E_PARAM;           /* our own key, or one that cannot be used */
    }
    n = node_find(c, key);
    if (n && n->kept >= 0) {
        return PCORE_OK;                /* idempotent: a game asserts its keeps whenever it opens */
    }
    for (i = 0; i < PCORE_KEPT; i++) {
        if (!c->kept_used[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return PCORE_E_FULL;
    }
    if (!n) {
        n = node_new(c, key, 1);
        if (!n) {
            return PCORE_E_FULL;
        }
        made = 1;
    }
    memset(&c->kept[slot], 0, sizeof(c->kept[slot]));
    memcpy(c->kept[slot].key, key, PC_KEY);
    c->kept[slot].addr = n->addr;
    c->kept[slot].port = n->port;
    /* What was already accepted from it - the invitation, the acceptance -
     * is the floor from the start, so none of it can be replayed after a
     * restart (S6). */
    c->kept[slot].epoch = n->win.epoch;
    c->kept[slot].mark = n->win.epoch ? n->win.top : 0;
    c->kept_used[slot] = 1;
    n->kept = (int8_t)slot;
    if (kept_write(c, c->kept_written) != 0) {
        /* Not kept unless it is on disk: the point of a kept peer is that
         * it is still one after a restart. */
        c->kept_used[slot] = 0;
        memset(&c->kept[slot], 0, sizeof(c->kept[slot]));
        n->kept = -1;
        if (made) {
            node_clear(n);
        }
        return PCORE_E_STORE;
    }
    /* Its datagrams already waiting are a kept peer's from now on (S5). */
    for (i = 0; i < PCORE_INBOX; i++) {
        if (c->inbox[i].used && memcmp(c->inbox[i].d.from, key, PC_KEY) == 0) {
            c->inbox[i].from_kept = 1;
        }
    }
    return PCORE_OK;
}

int pcore_release(struct pcore *c, const uint8_t key[PC_KEY])
{
    struct node *n = c && key ? node_find(c, key) : NULL;
    int slot;
    int nonkept = 0;
    int i;

    if (!n || n->kept < 0) {
        return PCORE_E_UNKNOWN;
    }
    slot = n->kept;
    c->kept_used[slot] = 0;
    n->kept = -1;
    if (kept_write(c, c->kept_written) != 0) {
        c->kept_used[slot] = 1;
        n->kept = (int8_t)slot;
        return PCORE_E_STORE;
    }
    memset(&c->kept[slot], 0, sizeof(c->kept[slot]));
    c->disk_epoch[slot] = 0;
    c->disk_mark[slot] = 0;
    for (i = 0; i < PCORE_INBOX; i++) {
        if (c->inbox[i].used && memcmp(c->inbox[i].d.from, key, PC_KEY) == 0) {
            c->inbox[i].from_kept = 0;
        }
    }
    flush_held(c);                      /* nothing of its to wait for any more */
    for (i = 0; i < PCORE_NODES; i++) {
        nonkept += c->nodes[i].used && c->nodes[i].kept < 0;
    }
    if (nonkept > NONKEPT_MAX) {
        node_clear(n);                  /* an ordinary node now, and there is no room for one more */
    }
    return PCORE_OK;
}

int pcore_kept(const struct pcore *c, struct ps_kept *out, int max)
{
    struct ps_kept recs[PCORE_KEPT];
    int n = c ? kept_pack(c, recs) : 0;

    if (n > max) {
        n = max;
    }
    if (n > 0) {
        memcpy(out, recs, (size_t)n * sizeof(recs[0]));
    }
    return n;
}

int pcore_send_app(struct pcore *c, const uint8_t to[PC_KEY], uint8_t port, const uint8_t *payload,
                   size_t n, int64_t now)
{
    uint8_t frame[PW_DATA_MAX];
    uint8_t pt[1 + PW_PAYLOAD_MAX];
    struct pw_header h;
    struct node *node;
    int rc;

    if (!c || !to || !payload || n < 1 || n > PW_PAYLOAD_MAX || port < PW_PORT_MIN ||
        port > PW_PORT_MAX || memcmp(to, c->id->pk, PC_KEY) == 0) {
        return PCORE_E_PARAM;
    }
    node = node_find(c, to);
    if (!node) {
        return PCORE_E_UNKNOWN;
    }
    if (node->kept < 0 && !pcore_visible(c, now)) {
        return PCORE_E_CLOSED;
    }
    if (node->kept >= 0 && (node->addr == 0 || node->heard < 0 || now - node->heard > PCORE_STALE_MS)) {
        lookup_start(c, node, now);     /* it may have moved; ask, and still try where it was */
    }
    if (node->addr == 0) {
        return PCORE_E_NO_ROUTE;
    }
    if (node_session(c, node) != 0) {
        return PCORE_E_PARAM;
    }
    memset(&h, 0, sizeof(h));
    h.type = PW_DATA;
    memcpy(h.sender, c->id->pk, PC_KEY);
    memcpy(h.recipient, node->key, PW_PREFIX);
    h.epoch = c->epoch;
    h.seq = next_seq(c);
    if (h.seq == 0) {
        return PCORE_E_SEND;
    }
    pw_build_header(&h, frame);
    pt[0] = port;
    memcpy(pt + 1, payload, n);
    rc = pc_seal(node->sess.tx, frame, PW_HEADER, pt, n + 1, frame + PW_HEADER,
                 frame + PW_HEADER + PW_NONCE);
    pc_wipe(pt, sizeof(pt));
    if (rc != 0) {
        return PCORE_E_SEND;
    }
    return transmit(c, node->addr, node->port ? node->port : c->udp_port, frame,
                    PW_HEADER + PW_NONCE + n + 1 + PW_TAG, 0) == 0
               ? PCORE_OK
               : PCORE_E_SEND;
}

int pcore_inbox(const struct pcore *c, uint8_t port, uint64_t after_id, struct pcore_dgram *out,
                int max)
{
    int n = 0;

    if (!c || !out) {
        return 0;
    }
    /* Oldest first: ids are handed out in delivery order. */
    while (n < max) {
        const struct slot *best = NULL;
        int i;

        for (i = 0; i < PCORE_INBOX; i++) {
            const struct slot *s = &c->inbox[i];

            if (s->used && !s->held && s->d.port == port && s->d.id > after_id &&
                (!best || s->d.id < best->d.id)) {
                best = s;
            }
        }
        if (!best) {
            break;
        }
        out[n++] = best->d;
        after_id = best->d.id;
    }
    return n;
}

int pcore_nodes(const struct pcore *c, struct pcore_node_info *out, int max, int64_t now)
{
    int n = 0;
    int i;

    for (i = 0; c && i < PCORE_NODES && n < max; i++) {
        const struct node *nd = &c->nodes[i];

        if (!nd->used) {
            continue;
        }
        memset(&out[n], 0, sizeof(out[n]));
        memcpy(out[n].key, nd->key, PC_KEY);
        memcpy(out[n].name, nd->name, sizeof(out[n].name));
        out[n].addr = nd->addr;
        out[n].port = nd->port;
        out[n].heard_ms = nd->heard;
        out[n].kept = nd->kept >= 0;
        out[n].proved = nd->proved;
        out[n].table = nd->table_on && nd->table_heard >= 0 &&
                       now - nd->table_heard <= PCORE_TABLE_FRESH_MS;
        out[n].t = nd->table;
        n++;
    }
    return n;
}

/* ---- lifecycle ------------------------------------------------------------------------- */

int pcore_set_name(struct pcore *c, const char *name)
{
    size_t len = name ? strlen(name) : 0;

    if (!c || len > PW_NAME_MAX || !pw_name_ok((const uint8_t *)name, len)) {
        return PCORE_E_PARAM;
    }
    memset(c->name, 0, sizeof(c->name));
    memcpy(c->name, name, len);
    return PCORE_OK;
}

const char *pcore_name(const struct pcore *c)
{
    return c ? c->name : "";
}

void pcore_set_ifaces(struct pcore *c, const struct pcore_iface *ifs, int n)
{
    if (!c) {
        return;
    }
    if (n < 0 || !ifs) {
        n = 0;
    }
    if (n > PCORE_IFACES) {
        n = PCORE_IFACES;
    }
    c->n_ifs = n;
    if (n > 0) {
        memcpy(c->ifs, ifs, (size_t)n * sizeof(ifs[0]));
    }
}

struct pcore *pcore_new(const struct pc_identity *id, uint64_t epoch, const char *name,
                        uint16_t udp_port, const struct pcore_ops *ops, void *user,
                        const struct ps_kept *kept, int kept_n)
{
    struct pcore *c;
    int i;

    if (!id || !ops || epoch == 0 || udp_port == 0 || kept_n < 0 || kept_n > PCORE_KEPT ||
        (kept_n > 0 && !kept)) {
        return NULL;
    }
    c = calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->id = id;
    c->ops = *ops;
    c->user = user;
    c->epoch = epoch;
    c->udp_port = udp_port;
    c->kept_written = -PCORE_KEPT_WRITE_MS;
    bucket_init(&c->verify_known, VERIFY_KNOWN_PER_S);
    bucket_init(&c->verify_unknown, VERIFY_UNKNOWN_PER_S);
    bucket_init(&c->replies, REPLY_PER_S);
    bucket_init(&c->replies_kept, REPLY_KEPT_PER_S);
    if (name && pcore_set_name(c, name) != PCORE_OK) {
        c->name[0] = '\0';
    }
    /* Kept peers are nodes from the first instant: their next datagram is
     * accepted with no beacon and no round trip, and nothing at or below
     * their stored floor is accepted again. */
    for (i = 0; i < kept_n; i++) {
        struct node *n;

        if (!pc_peer_ok(id, kept[i].key) || node_find(c, kept[i].key)) {
            continue;
        }
        n = node_new(c, kept[i].key, 1);
        if (!n) {
            break;
        }
        c->kept[i] = kept[i];
        c->kept_used[i] = 1;
        c->disk_epoch[i] = kept[i].epoch;       /* as read: that is what is on disk */
        c->disk_mark[i] = kept[i].mark;
        n->kept = (int8_t)i;
        n->addr = kept[i].addr;
        n->port = kept[i].port;
        n->floor_loaded = kept[i].epoch != 0;
        pr_restore(&n->win, kept[i].epoch, kept[i].mark);
    }
    return c;
}

void pcore_free(struct pcore *c)
{
    int i;

    if (!c) {
        return;
    }
    for (i = 0; i < PCORE_NODES; i++) {
        pc_session_wipe(&c->nodes[i].sess);
    }
    pc_wipe(c->inbox, sizeof(c->inbox));
    free(c);
}

uint64_t pcore_counter(const struct pcore *c, enum pcore_count which)
{
    return c && which < PCORE_C_COUNT ? c->count[which] : 0;
}

const char *pcore_counter_name(enum pcore_count which)
{
    static const char *const names[PCORE_C_COUNT] = {
        "rx", "drop_malformed", "drop_own", "drop_other", "drop_unknown", "drop_not_visible",
        "drop_stated", "drop_replay", "drop_budget", "drop_bad_key", "drop_auth", "drop_inbox_full",
        "accepted_data", "accepted_signed", "tx", "tx_failed", "reply_limited", "kept_writes",
        "kept_write_failed", "evicted_nodes",
    };

    return which < PCORE_C_COUNT ? names[which] : "?";
}

uint64_t pcore_epoch(const struct pcore *c)
{
    return c ? c->epoch : 0;
}

uint64_t pcore_sequence(const struct pcore *c)
{
    return c ? c->seq : 0;
}

int pcore_kept_write_pending(const struct pcore *c)
{
    return c ? c->kept_dirty : 0;
}

#ifdef PEER_CORE_TEST
void pcore_test_set_sequence(struct pcore *c, uint64_t seq)
{
    c->seq = seq;
}
#endif

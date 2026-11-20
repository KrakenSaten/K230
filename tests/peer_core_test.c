/*
 * peerd's engine (services/peerd/peer_core.h) on a network the test owns:
 * several engines wired back to back, every packet in a queue the test can
 * read, drop, alter, hold back, replay or deliver from another address.
 *
 * These are the negative tests ADR-017 names ("Validation before
 * implementation carries a game packet", item 5) and its restart and
 * recovery behaviour (section 8), each under the name of the rule it holds
 * the code to.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "peer_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

/* ---- the network ------------------------------------------------------------------- */

#define N 8
#define QMAX 4096
#define PORT 47230
#define NET 0x0A000000u         /* 10.0.0.0/24 */
#define BCAST (NET | 255)

struct sim {
    struct pc_identity id;
    struct pcore *core;
    uint32_t addr;
    uint64_t epoch;             /* the epoch file */
    struct ps_kept disk[PS_KEPT_MAX];   /* kept.v1 */
    int disk_n;
    int save_fail;
    int saves;
    int delivered;
    uint64_t last_id;
    char name[16];
};

struct pkt {
    int from;
    uint32_t src;
    uint32_t dst;
    uint16_t dport;
    int bcast;
    size_t n;
    uint8_t b[PW_FRAME_MAX];
};

static struct sim node[N];
static struct pkt q[QMAX];
static int qn;
static int64_t now = 100000;

static int op_send(void *user, uint32_t dst, uint16_t port, const uint8_t *buf, size_t n, int bcast)
{
    struct sim *s = user;
    struct pkt *p;

    if (qn == QMAX || n > PW_FRAME_MAX) {
        return -1;
    }
    p = &q[qn++];
    p->from = (int)(s - node);
    p->src = s->addr;
    p->dst = dst;
    p->dport = port;
    p->bcast = bcast;
    p->n = n;
    memcpy(p->b, buf, n);
    return 0;
}

static uint32_t op_source_for(void *user, uint32_t dst)
{
    (void)dst;
    return ((struct sim *)user)->addr;
}

static int op_kept_save(void *user, const struct ps_kept *recs, int n)
{
    struct sim *s = user;

    if (s->save_fail) {
        return -1;
    }
    s->disk_n = n;
    if (n > 0) {
        memcpy(s->disk, recs, (size_t)n * sizeof(recs[0]));
    }
    s->saves++;
    return 0;
}

static void op_delivered(void *user, const struct pcore_dgram *d)
{
    struct sim *s = user;

    s->delivered++;
    s->last_id = d->id;
}

static const struct pcore_ops OPS = { op_send, op_source_for, op_kept_save, op_delivered };

/* Start (or start again) a node's engine from what is "on disk": the same
 * identity, the next epoch, the kept list as last written. */
static void boot(int i)
{
    struct sim *s = &node[i];
    struct pcore_iface ifc;

    if (s->core) {
        pcore_free(s->core);
    }
    s->epoch++;
    s->core = pcore_new(&s->id, s->epoch, s->name, PORT, &OPS, s, s->disk, s->disk_n);
    ifc.addr = s->addr;
    ifc.bcast = BCAST;
    pcore_set_ifaces(s->core, &ifc, 1);
    s->delivered = 0;
    s->last_id = 0;
}

static void world(void)
{
    int i;

    for (i = 0; i < N; i++) {
        struct sim *s = &node[i];

        if (s->core) {
            pcore_free(s->core);
        }
        pc_identity_wipe(&s->id);
        memset(s, 0, sizeof(*s));
        pc_identity_generate(&s->id);
        s->addr = NET | (uint32_t)(10 + i);
        snprintf(s->name, sizeof(s->name), "node%d", i);
        s->epoch = 1;
        boot(i);
    }
    qn = 0;
    now += 1000000;     /* a new world is a long time after the last */
}

static void remove_at(int i)
{
    memmove(&q[i], &q[i + 1], (size_t)(qn - i - 1) * sizeof(q[0]));
    qn--;
}

static void input(int to, uint32_t src, const uint8_t *b, size_t n)
{
    pcore_input(node[to].core, src, PORT, b, n, now);
}

static void deliver(const struct pkt *p)
{
    int i;

    for (i = 0; i < N; i++) {
        if (i == p->from || !node[i].core) {
            continue;
        }
        if (p->bcast ? p->dst == BCAST : node[i].addr == p->dst) {
            input(i, p->src, p->b, p->n);
        }
    }
}

/* Deliver everything, and everything that draws in reply. */
static void flush(void)
{
    int guard = 0;

    while (qn > 0 && guard++ < 100000) {
        struct pkt p = q[0];

        remove_at(0);
        deliver(&p);
    }
}

static void drop_all(void)
{
    qn = 0;
}

static void tick_all(void)
{
    int i;

    for (i = 0; i < N; i++) {
        if (node[i].core) {
            pcore_tick(node[i].core, now);
        }
    }
}

static void step(int64_t ms)
{
    now += ms;
    tick_all();
    flush();
}

static uint64_t cnt(int i, enum pcore_count c)
{
    return pcore_counter(node[i].core, c);
}

static void visible(int i)
{
    pcore_set_visible(node[i].core, 1, now);
}

/* Two nodes that have heard each other's beacons: a whole beacon interval,
 * so one that was already visible announces itself again too. */
static void meet(int a, int b)
{
    visible(a);
    visible(b);
    step(PCORE_BEACON_MS);
}

static int send_text(int from, int to, uint8_t port, const char *text)
{
    return pcore_send_app(node[from].core, node[to].id.pk, port, (const uint8_t *)text, strlen(text), now);
}

static int inbox_n(int i, uint8_t port, uint64_t after)
{
    static struct pcore_dgram out[PCORE_INBOX];

    return pcore_inbox(node[i].core, port, after, out, PCORE_INBOX);
}

static int inbox_has(int i, uint8_t port, const char *text)
{
    static struct pcore_dgram out[PCORE_INBOX];
    int n = pcore_inbox(node[i].core, port, 0, out, PCORE_INBOX);
    int k;

    for (k = 0; k < n; k++) {
        if (out[k].len == strlen(text) && memcmp(out[k].data, text, out[k].len) == 0) {
            return 1;
        }
    }
    return 0;
}

static int node_info(int i, int about, struct pcore_node_info *out)
{
    static struct pcore_node_info info[PCORE_NODES];
    int n = pcore_nodes(node[i].core, info, PCORE_NODES, now);
    int k;

    for (k = 0; k < n; k++) {
        if (memcmp(info[k].key, node[about].id.pk, PC_KEY) == 0) {
            if (out) {
                *out = info[k];
            }
            return 1;
        }
    }
    return 0;
}

static int node_count(int i)
{
    static struct pcore_node_info info[PCORE_NODES];

    return pcore_nodes(node[i].core, info, PCORE_NODES, now);
}

/* The index of the first queued packet of a type from a node, or -1. */
static int find(int from, uint8_t type)
{
    int i;

    for (i = 0; i < qn; i++) {
        if (q[i].from == from && q[i].b[1] == type) {
            return i;
        }
    }
    return -1;
}

/* Send one datagram and take it out of the queue instead of delivering it. */
static struct pkt capture(int from, int to, const char *text)
{
    struct pkt p;
    int at;

    memset(&p, 0, sizeof(p));
    send_text(from, to, 1, text);
    at = find(from, PW_DATA);
    if (at >= 0) {
        p = q[at];
        remove_at(at);
    }
    return p;
}

/* ---- silence, discovery, a datagram ---------------------------------------------------- */

static void test_silence_and_discovery(void)
{
    struct pcore_node_info info;
    uint32_t tag = 0;

    world();
    step(3000);
    check("silence: an engine nobody asked anything of sends nothing",
          qn == 0 && cnt(0, PCORE_C_TX) == 0 && !pcore_wants_socket(node[0].core, now));
    visible(0);
    check("silence: visible, it wants a socket", pcore_wants_socket(node[0].core, now) &&
          pcore_visible(node[0].core, now));
    now += 0;
    tick_all();
    check("discovery: it announces itself at once, to its link's broadcast address",
          qn == 1 && q[0].bcast && q[0].dst == BCAST && q[0].n == PW_SIGNED_SIZE && q[0].b[1] == PW_BEACON);
    flush();
    check("discovery: a node that is not visible does not hear a stranger",
          !node_info(1, 0, NULL) && cnt(1, PCORE_C_NOT_VISIBLE) == 1);
    visible(1);
    step(2000);
    check("discovery: two visible nodes list each other", node_info(0, 1, &info) && node_info(1, 0, NULL));
    check("discovery: by name, with an address, and not yet proved live",
          strcmp(info.name, "node1") == 0 && info.addr == node[1].addr && info.port == PORT &&
          info.proved == 0 && info.kept == 0 && info.heard_ms == now);
    check("discovery: a beacon every two seconds, not more", ({
              uint64_t before = cnt(0, PCORE_C_TX);

              step(1000);
              step(1000);
              step(1000);
              step(1000);
              cnt(0, PCORE_C_TX) - before == 2;
          }));
    check("discovery: a table is announced in the beacon",
          pcore_announce(node[0].core, 1, 1, 1, 2, &tag) == PCORE_OK && tag != 0);
    step(2000);
    check("discovery: and seen, with its fields", node_info(1, 0, &info) && info.table &&
          info.t.port == 1 && info.t.seats == 2 && info.t.tag == tag);
    pcore_withdraw(node[0].core);
    step(2000);
    check("discovery: withdrawn, it is gone at the next beacon", node_info(1, 0, &info) && !info.table);
    pcore_announce(node[0].core, 1, 1, 1, 2, &tag);
    step(2000);
    pcore_set_visible(node[0].core, 0, now);
    step(PCORE_TABLE_FRESH_MS + 2000);
    check("discovery: a table not repeated for ten seconds is no longer listed",
          node_info(1, 0, &info) && !info.table);
    check("discovery: a bad announcement is refused",
          pcore_announce(node[0].core, 0, 1, 1, 2, NULL) == PCORE_E_PARAM &&
          pcore_announce(node[0].core, 16, 1, 1, 2, NULL) == PCORE_E_PARAM &&
          pcore_announce(node[0].core, 1, 2, 1, 2, NULL) == PCORE_E_PARAM &&
          pcore_announce(node[0].core, 1, 1, 1, 0, NULL) == PCORE_E_PARAM);
    step(PCORE_LEASE_MS);
    check("lease: thirty seconds after it was last renewed, visibility has lapsed",
          !pcore_visible(node[0].core, now) && pcore_visible(node[1].core, now) == 0);
    {
        uint64_t before = cnt(1, PCORE_C_TX);

        step(5000);
        check("lease: and nothing more is announced", cnt(1, PCORE_C_TX) == before &&
              !pcore_wants_socket(node[1].core, now));
    }
}

static void test_datagram(void)
{
    static struct pcore_dgram out[PCORE_INBOX];
    struct pcore_node_info info;

    world();
    meet(0, 1);
    check("datagram: sent to a node that was heard", send_text(0, 1, 1, "hello") == PCORE_OK);
    check("datagram: it is one sealed frame, to that node's address",
          qn == 1 && !q[0].bcast && q[0].dst == node[1].addr && q[0].n == PW_DATA_MIN + 4 &&
          q[0].b[1] == PW_DATA);
    check("datagram: the words are not in it", ({
              int found = 0;
              size_t i;

              for (i = 0; i + 5 <= q[0].n; i++) {
                  found |= memcmp(q[0].b + i, "hello", 5) == 0;
              }
              !found;
          }));
    flush();
    check("datagram: it arrives, once, with its port, its sender and its bytes",
          pcore_inbox(node[1].core, 1, 0, out, PCORE_INBOX) == 1 && out[0].id == 1 && out[0].port == 1 &&
          out[0].len == 5 && memcmp(out[0].data, "hello", 5) == 0 &&
          memcmp(out[0].from, node[0].id.pk, PC_KEY) == 0 && node[1].delivered == 1);
    check("datagram: another port's inbox does not hold it", inbox_n(1, 2, 0) == 0);
    check("datagram: from a node that is not kept it proves nothing - it could be a recording",
          node_info(1, 0, &info) && !info.proved);
    check("datagram: and the answer comes back", send_text(1, 0, 1, "hi") == PCORE_OK && (flush(), 1) &&
          inbox_has(0, 1, "hi"));
    send_text(0, 1, 1, "two");
    send_text(0, 1, 3, "three");
    flush();
    check("datagram: ids rise in the order delivered, and after_id skips what was read",
          pcore_inbox(node[1].core, 1, 0, out, PCORE_INBOX) == 2 && out[0].id == 1 && out[1].id == 2 &&
          pcore_inbox(node[1].core, 1, 1, out, PCORE_INBOX) == 1 && out[0].id == 2 &&
          pcore_inbox(node[1].core, 3, 0, out, PCORE_INBOX) == 1 && out[0].id == 3);
    {
        static uint8_t big[PW_PAYLOAD_MAX + 1];

        memset(big, 0x5A, sizeof(big));
        check("datagram: 1024 bytes is the most, and it arrives whole",
              pcore_send_app(node[0].core, node[1].id.pk, 2, big, PW_PAYLOAD_MAX, now) == PCORE_OK &&
              q[0].n == PW_DATA_MAX && (flush(), 1) &&
              pcore_inbox(node[1].core, 2, 0, out, PCORE_INBOX) == 1 && out[0].len == PW_PAYLOAD_MAX &&
              out[0].data[1023] == 0x5A);
        check("datagram: 1025 bytes, none, port 0 and port 16 are refused",
              pcore_send_app(node[0].core, node[1].id.pk, 2, big, PW_PAYLOAD_MAX + 1, now) == PCORE_E_PARAM &&
              pcore_send_app(node[0].core, node[1].id.pk, 2, big, 0, now) == PCORE_E_PARAM &&
              pcore_send_app(node[0].core, node[1].id.pk, 0, big, 4, now) == PCORE_E_PARAM &&
              pcore_send_app(node[0].core, node[1].id.pk, 16, big, 4, now) == PCORE_E_PARAM);
    }
    check("datagram: to a key that is not a node, nothing goes",
          send_text(0, 2, 1, "x") == PCORE_E_UNKNOWN && qn == 0);
    check("datagram: to this device's own key, nothing goes",
          pcore_send_app(node[0].core, node[0].id.pk, 1, (const uint8_t *)"x", 1, now) == PCORE_E_PARAM);
    pcore_set_visible(node[0].core, 0, now);
    check("datagram: not visible, nothing is sent to a node that is not kept",
          send_text(0, 1, 1, "x") == PCORE_E_CLOSED && qn == 0);
}

/* ---- forged, altered, reflected, re-addressed ---------------------------------------------- */

static void test_forgeries(void)
{
    struct pkt p;
    struct pkt alt;
    size_t i;
    uint64_t accepted;
    int bad;

    world();
    meet(0, 1);
    meet(0, 2);
    meet(1, 2);
    p = capture(0, 1, "attack at dawn");
    check("forgery: (the genuine frame was captured)", p.n == PW_DATA_MIN + 13);

    /* Reflected: back to its own sender. */
    input(0, node[1].addr, p.b, p.n);
    check("reflected: a datagram sent back to its sender is dropped as its own",
          cnt(0, PCORE_C_OWN) >= 1 && inbox_n(0, 1, 0) == 0);
    alt = p;
    memcpy(alt.b + 2, node[1].id.pk, PC_KEY);
    memcpy(alt.b + 34, node[0].id.pk, PW_PREFIX);
    accepted = cnt(0, PCORE_C_ACCEPTED_DATA);
    input(0, node[1].addr, alt.b, alt.n);
    check("reflected: relabelled as the peer's and sent back, it does not open",
          cnt(0, PCORE_C_AUTH) == 1 && cnt(0, PCORE_C_ACCEPTED_DATA) == accepted && inbox_n(0, 1, 0) == 0);

    /* Re-addressed: to a third node that knows the sender. */
    input(2, node[0].addr, p.b, p.n);
    check("re-addressed: handed to another node as it is, it is not for that node",
          cnt(2, PCORE_C_OTHER) == 1 && inbox_n(2, 1, 0) == 0);
    alt = p;
    memcpy(alt.b + 34, node[2].id.pk, PW_PREFIX);
    input(2, node[0].addr, alt.b, alt.n);
    check("re-addressed: with the recipient rewritten to that node, it does not open",
          cnt(2, PCORE_C_AUTH) == 1 && inbox_n(2, 1, 0) == 0);

    /* Every header byte, one at a time. */
    accepted = cnt(1, PCORE_C_ACCEPTED_DATA);
    bad = 0;
    for (i = 0; i < PW_HEADER; i++) {
        alt = p;
        alt.b[i] ^= 0x01;
        input(1, node[0].addr, alt.b, alt.n);
        bad += cnt(1, PCORE_C_ACCEPTED_DATA) != accepted;
    }
    check("altered header: any one of the 58 header bytes changed, and nothing is accepted",
          bad == 0 && inbox_n(1, 1, 0) == 0);
    bad = 0;
    for (i = PW_HEADER; i < p.n; i++) {
        alt = p;
        alt.b[i] ^= 0x80;
        input(1, node[0].addr, alt.b, alt.n);
        bad += cnt(1, PCORE_C_ACCEPTED_DATA) != accepted;
    }
    check("altered body: any nonce, ciphertext or tag byte changed, and nothing is accepted",
          bad == 0 && inbox_n(1, 1, 0) == 0 && cnt(1, PCORE_C_AUTH) >= p.n - PW_HEADER);
    alt = p;
    check("truncated: cut by one byte, or grown by one, it is not accepted", ({
              input(1, node[0].addr, alt.b, alt.n - 1);
              alt.b[alt.n] = 0;
              input(1, node[0].addr, alt.b, alt.n + 1);
              cnt(1, PCORE_C_ACCEPTED_DATA) == accepted;
          }));
    /* None of that spoiled the genuine frame: it is still accepted, once. */
    input(1, node[0].addr, p.b, p.n);
    check("genuine: after all of it, the untouched frame is accepted",
          cnt(1, PCORE_C_ACCEPTED_DATA) == accepted + 1 && inbox_has(1, 1, "attack at dawn"));
    input(1, node[0].addr, p.b, p.n);
    check("replay: and a second copy of it is not", cnt(1, PCORE_C_ACCEPTED_DATA) == accepted + 1 &&
          cnt(1, PCORE_C_REPLAY) >= 1 && inbox_n(1, 1, 0) == 1);
    /* Epoch and sequence zero cannot even be parsed. */
    alt = p;
    memset(alt.b + 50, 0, 8);
    input(1, node[0].addr, alt.b, alt.n);
    alt = p;
    memset(alt.b + 42, 0, 8);
    input(1, node[0].addr, alt.b, alt.n);
    check("sequence 0, epoch 0: refused as malformed", cnt(1, PCORE_C_MALFORMED) >= 2);
    /* Rubbish. */
    {
        static uint8_t junk[PW_FRAME_MAX + 50];
        uint64_t rx = cnt(1, PCORE_C_RX);

        memset(junk, 0xFF, sizeof(junk));
        input(1, node[0].addr, junk, 0);
        input(1, node[0].addr, junk, 1);
        input(1, node[0].addr, junk, PW_HEADER);
        input(1, node[0].addr, junk, sizeof(junk));
        check("rubbish: nothing, one byte, a bare header and an oversized datagram are all dropped",
              cnt(1, PCORE_C_RX) == rx + 4 && cnt(1, PCORE_C_ACCEPTED_DATA) == accepted + 1);
    }
}

/* ---- replay ------------------------------------------------------------------------------- */

static void test_replay(void)
{
    struct pkt p[80];
    int i;
    uint64_t accepted;

    world();
    meet(0, 1);
    for (i = 0; i < 5; i++) {
        char text[8];

        snprintf(text, sizeof(text), "m%d", i);
        p[i] = capture(0, 1, text);
    }
    input(1, node[0].addr, p[4].b, p[4].n);
    input(1, node[0].addr, p[2].b, p[2].n);
    check("replay: out of order inside the window, both are accepted",
          inbox_has(1, 1, "m4") && inbox_has(1, 1, "m2"));
    accepted = cnt(1, PCORE_C_ACCEPTED_DATA);
    input(1, node[0].addr, p[2].b, p[2].n);
    input(1, node[0].addr, p[4].b, p[4].n);
    check("replay: a copy of either is dropped", cnt(1, PCORE_C_ACCEPTED_DATA) == accepted &&
          cnt(1, PCORE_C_REPLAY) == 2);
    input(1, node[0].addr, p[0].b, p[0].n);
    check("replay: one that was missed is still accepted later", inbox_has(1, 1, "m0"));

    /* Below the window: held back behind more than 64 later frames. */
    for (i = 0; i < 70; i++) {
        p[i] = capture(0, 1, "late");
    }
    for (i = 1; i < 70; i++) {
        input(1, node[0].addr, p[i].b, p[i].n);
    }
    accepted = cnt(1, PCORE_C_ACCEPTED_DATA);
    input(1, node[0].addr, p[0].b, p[0].n);
    check("replay: a frame held back behind more than 64 later ones is dropped",
          cnt(1, PCORE_C_ACCEPTED_DATA) == accepted);
    input(1, node[0].addr, p[10].b, p[10].n);
    check("replay: and one of those 64 is not accepted twice", cnt(1, PCORE_C_ACCEPTED_DATA) == accepted);

    /* A jump of far more than 64. */
    for (i = 0; i < 200; i++) {
        capture(0, 1, "skipped");
    }
    send_text(0, 1, 1, "after the gap");
    flush();
    check("replay: after a gap of two hundred lost frames the next is accepted",
          inbox_has(1, 1, "after the gap"));
}

static void test_lower_epoch(void)
{
    struct pkt old;
    uint64_t accepted;

    world();
    meet(0, 1);
    send_text(0, 1, 1, "first run");
    flush();
    old = capture(0, 1, "old epoch, never delivered");
    boot(0);                    /* node 0 restarts: the next epoch, sequence from 1 */
    visible(0);
    step(PCORE_BEACON_MS);
    send_text(0, 1, 1, "second run");
    flush();
    check("epoch: a restarted sender's first datagram is accepted, sequence 1 and all",
          inbox_has(1, 1, "second run"));
    accepted = cnt(1, PCORE_C_ACCEPTED_DATA);
    input(1, node[0].addr, old.b, old.n);
    check("lower epoch: a frame of the run before, never seen, is dropped all the same",
          cnt(1, PCORE_C_ACCEPTED_DATA) == accepted && !inbox_has(1, 1, "old epoch, never delivered"));
}

/* ---- kept peers: restart, recovery, the replay floor ------------------------------------------ */

static void test_kept_restart(void)
{
    struct pkt m1;
    struct pkt m2;
    struct pcore_node_info info;
    uint64_t accepted;

    world();
    meet(0, 1);
    check("keep: each keeps the other", pcore_keep(node[0].core, node[1].id.pk) == PCORE_OK &&
          pcore_keep(node[1].core, node[0].id.pk) == PCORE_OK);
    check("keep: it is on disk at once", node[0].disk_n == 1 && node[1].disk_n == 1 &&
          memcmp(node[1].disk[0].key, node[0].id.pk, PC_KEY) == 0);
    check("keep: asserted again it is still one record, and no error",
          pcore_keep(node[1].core, node[0].id.pk) == PCORE_OK && node[1].disk_n == 1);
    send_text(0, 1, 1, "move 1");
    m1 = q[0];
    flush();
    step(PCORE_KEPT_WRITE_MS);
    send_text(0, 1, 1, "move 2");
    m2 = q[0];
    flush();
    step(PCORE_KEPT_WRITE_MS);
    check("keep: both moves arrived", inbox_has(1, 1, "move 1") && inbox_has(1, 1, "move 2"));
    check("keep: and the replay floor on disk is the second one's",
          node[1].disk[0].epoch == node[0].epoch && node[1].disk[0].mark == pw_get64(m2.b + 50));

    /* The receiver restarts. Nobody is visible; nothing is announced. */
    pcore_set_visible(node[0].core, 0, now);
    boot(1);
    check("restart: with a kept peer it wants its socket at once, visible or not",
          pcore_wants_socket(node[1].core, now) && !pcore_visible(node[1].core, now));
    check("restart: the kept peer is a node from the first instant, with its last address",
          node_info(1, 0, &info) && info.kept && info.addr == node[0].addr && info.heard_ms == -1);
    accepted = cnt(1, PCORE_C_ACCEPTED_DATA);
    input(1, node[0].addr, m1.b, m1.n);
    input(1, node[0].addr, m2.b, m2.n);
    check("restart: the opponent's earlier moves, replayed into it, are dropped",
          cnt(1, PCORE_C_ACCEPTED_DATA) == accepted && cnt(1, PCORE_C_REPLAY) == 2 &&
          inbox_n(1, 1, 0) == 0);
    check("restart: the opponent's next move is accepted with no beacon and no round trip",
          send_text(0, 1, 1, "move 3") == PCORE_OK && (flush(), 1) && inbox_has(1, 1, "move 3"));
    check("restart: and it can answer, to the address it remembered",
          send_text(1, 0, 1, "reply 3") == PCORE_OK && (flush(), 1) && inbox_has(0, 1, "reply 3"));

    /* The sender restarts: a new epoch, a low sequence (review P1). */
    boot(0);
    send_text(0, 1, 1, "new epoch, sequence 1");
    m1 = q[find(0, PW_DATA)];
    flush();
    step(PCORE_KEPT_WRITE_MS);
    check("new epoch: the restarted opponent's first datagram is accepted and its floor persisted",
          inbox_has(1, 1, "new epoch, sequence 1") && node[1].disk[0].epoch == node[0].epoch &&
          node[1].disk[0].mark == pw_get64(m1.b + 50));
    boot(1);
    accepted = cnt(1, PCORE_C_ACCEPTED_DATA);
    input(1, node[0].addr, m1.b, m1.n);
    input(1, node[0].addr, m2.b, m2.n);
    check("new epoch: after another restart neither that datagram nor an older epoch's is accepted",
          cnt(1, PCORE_C_ACCEPTED_DATA) == accepted && inbox_n(1, 1, 0) == 0);
}

/* A datagram accepted inside the window, behind a beacon, must still be
 * covered by the floor (review N1). */
static void test_data_behind_beacon(void)
{
    struct pkt d;
    uint64_t accepted;

    world();
    meet(0, 1);
    pcore_keep(node[1].core, node[0].id.pk);
    pcore_keep(node[0].core, node[1].id.pk);
    d = capture(0, 1, "behind the beacon");     /* sequence k */
    step(PCORE_BEACON_MS);                      /* node 0's beacon, sequence k+1, arrives first */
    input(1, node[0].addr, d.b, d.n);
    step(PCORE_KEPT_WRITE_MS);
    check("behind a beacon: the datagram is accepted inside the window",
          inbox_has(1, 1, "behind the beacon"));
    check("behind a beacon: and it set the floor, though it did not move the window's top",
          node[1].disk[0].mark == pw_get64(d.b + 50));
    boot(1);
    accepted = cnt(1, PCORE_C_ACCEPTED_DATA);
    input(1, node[0].addr, d.b, d.n);
    check("behind a beacon: replayed after a restart, it is dropped",
          cnt(1, PCORE_C_ACCEPTED_DATA) == accepted && inbox_n(1, 1, 0) == 0);
}

static void test_kept_write(void)
{
    world();
    meet(0, 1);
    pcore_keep(node[1].core, node[0].id.pk);
    node[1].saves = 0;
    step(PCORE_KEPT_WRITE_MS);
    send_text(0, 1, 1, "a");
    flush();
    check("write: the first datagram's floor is written, and it is delivered at once",
          node[1].saves == 1 && node[1].delivered == 1 && inbox_has(1, 1, "a"));
    send_text(0, 1, 1, "b");
    send_text(0, 1, 1, "c");
    flush();
    check("write: two more within a quarter second are held, not written one by one",
          node[1].saves == 1 && node[1].delivered == 1 && !inbox_has(1, 1, "b") &&
          pcore_kept_write_pending(node[1].core));
    step(PCORE_KEPT_WRITE_MS);
    check("write: one write then covers both, and they are delivered in the order they came",
          node[1].saves == 2 && node[1].delivered == 3 && inbox_n(1, 1, 0) == 3 && ({
              static struct pcore_dgram out[PCORE_INBOX];

              pcore_inbox(node[1].core, 1, 0, out, PCORE_INBOX);
              out[1].data[0] == 'b' && out[2].data[0] == 'c' && out[1].id == 2 && out[2].id == 3;
          }));
    /* A write that fails: the datagram waits; it is not delivered without its floor. */
    node[1].save_fail = 1;
    step(PCORE_KEPT_WRITE_MS);
    send_text(0, 1, 1, "d");
    flush();
    step(PCORE_KEPT_WRITE_MS);
    step(PCORE_KEPT_WRITE_MS);
    check("write: while the list cannot be written the datagram is held back",
          !inbox_has(1, 1, "d") && node[1].delivered == 3 && cnt(1, PCORE_C_KEPT_WRITE_FAILED) >= 1);
    node[1].save_fail = 0;
    step(PCORE_KEPT_WRITE_MS);
    check("write: and delivered as soon as it can be", inbox_has(1, 1, "d") && node[1].delivered == 4);

    /* Keep and release. */
    world();
    node[0].save_fail = 1;
    check("keep: a peer that cannot be put on disk is not kept",
          pcore_keep(node[0].core, node[1].id.pk) == PCORE_E_STORE && node[0].disk_n == 0 &&
          node_count(0) == 0 && !pcore_wants_socket(node[0].core, now));
    node[0].save_fail = 0;
    check("keep: four are kept", pcore_keep(node[0].core, node[1].id.pk) == PCORE_OK &&
          pcore_keep(node[0].core, node[2].id.pk) == PCORE_OK &&
          pcore_keep(node[0].core, node[3].id.pk) == PCORE_OK &&
          pcore_keep(node[0].core, node[4].id.pk) == PCORE_OK && node[0].disk_n == 4);
    check("keep: a fifth is refused", pcore_keep(node[0].core, node[5].id.pk) == PCORE_E_FULL &&
          node[0].disk_n == 4);
    check("keep: this device's own key is refused", pcore_keep(node[0].core, node[0].id.pk) == PCORE_E_PARAM);
    {
        uint8_t small[PC_KEY];
        uint8_t mixed[PC_KEY] = { 0x3b, 0x5b, 0x47, 0x5c, 0x4b, 0x82, 0xdd, 0x15, 0x72, 0x79, 0x9f,
                                  0xc5, 0x46, 0xf4, 0xc6, 0xc0, 0x3e, 0x47, 0x8c, 0x66, 0x54, 0xaa,
                                  0x4c, 0x7f, 0x94, 0x5b, 0x34, 0x7e, 0xa3, 0x2a, 0xf6, 0x0d };

        memset(small, 0, sizeof(small));
        small[0] = 1;
        pcore_release(node[0].core, node[4].id.pk);
        check("keep: a small-order key and a mixed-order key are refused",
              pcore_keep(node[0].core, small) == PCORE_E_PARAM &&
              pcore_keep(node[0].core, mixed) == PCORE_E_PARAM && node[0].disk_n == 3);
    }
    check("release: one is released, on disk too", pcore_release(node[0].core, node[2].id.pk) == PCORE_OK &&
          node[0].disk_n == 2);
    check("release: what was not kept cannot be released",
          pcore_release(node[0].core, node[2].id.pk) == PCORE_E_UNKNOWN &&
          pcore_release(node[0].core, node[6].id.pk) == PCORE_E_UNKNOWN);
    {
        struct ps_kept list[PCORE_KEPT];

        check("kept: the list is what is kept", pcore_kept(node[0].core, list, PCORE_KEPT) == 2);
    }
    pcore_release(node[0].core, node[1].id.pk);
    pcore_release(node[0].core, node[3].id.pk);
    check("release: with none left and not visible, no socket is wanted",
          node[0].disk_n == 0 && !pcore_wants_socket(node[0].core, now));
}

/* ---- the floor is what is on disk (review S1, S2, S5, S6) ------------------------------------- */

/* A datagram is delivered only once a floor at or above it is on disk - not
 * merely in memory, and not because a newer one is already waiting. */
static void test_floor_on_disk(void)
{
    struct pkt older;
    uint64_t accepted;

    world();
    meet(0, 1);
    pcore_keep(node[1].core, node[0].id.pk);
    pcore_keep(node[0].core, node[1].id.pk);
    step(PCORE_KEPT_WRITE_MS);
    send_text(0, 1, 1, "first");
    flush();                                    /* written and delivered: the floor on disk */
    older = capture(0, 1, "older, delayed");    /* sequence k */
    send_text(0, 1, 1, "newer");                /* sequence k+1, within the write cap */
    flush();
    check("floor: a datagram inside the write cap waits; its floor is only in memory",
          node[1].delivered == 1 && !inbox_has(1, 1, "newer"));
    input(1, node[0].addr, older.b, older.n);   /* the delayed one, inside the window */
    check("floor: an older one arriving beside it waits too - it is above what is on disk",
          node[1].delivered == 1 && !inbox_has(1, 1, "older, delayed"));
    /* Power is cut here: no tick, no write. */
    boot(1);
    check("floor: after a power cut neither had been delivered", node[1].delivered == 0);
    accepted = cnt(1, PCORE_C_ACCEPTED_DATA);
    input(1, node[0].addr, older.b, older.n);
    step(PCORE_KEPT_WRITE_MS);
    check("floor: so arriving again it is accepted - once, not a second time",
          cnt(1, PCORE_C_ACCEPTED_DATA) == accepted + 1 && inbox_n(1, 1, 0) == 1 &&
          inbox_has(1, 1, "older, delayed"));

    /* While the list cannot be written, nothing above the disk floor leaks out. */
    world();
    meet(0, 1);
    pcore_keep(node[1].core, node[0].id.pk);
    pcore_keep(node[0].core, node[1].id.pk);
    step(PCORE_KEPT_WRITE_MS);
    send_text(0, 1, 1, "on disk");
    flush();
    node[1].save_fail = 1;
    older = capture(0, 1, "older");
    step(PCORE_KEPT_WRITE_MS);
    send_text(0, 1, 1, "newer");
    flush();
    step(PCORE_KEPT_WRITE_MS);
    input(1, node[0].addr, older.b, older.n);
    step(PCORE_KEPT_WRITE_MS);
    check("floor: with the disk refusing writes, neither the newer nor the older one is delivered",
          node[1].delivered == 1 && inbox_n(1, 1, 0) == 1);
    node[1].save_fail = 0;
    step(PCORE_KEPT_WRITE_MS);
    check("floor: and both are, in the order they came, once it can be written",
          node[1].delivered == 3 && ({
              static struct pcore_dgram out[PCORE_INBOX];

              pcore_inbox(node[1].core, 1, 0, out, PCORE_INBOX) == 3 && out[1].data[0] == 'n' &&
              out[2].data[0] == 'o' && out[2].len == 5;
          }));
}

/* Nothing a write covers is left waiting, whoever caused the write. */
static void test_held_is_flushed(void)
{
    world();
    meet(0, 1);
    meet(0, 2);
    pcore_keep(node[1].core, node[0].id.pk);
    pcore_keep(node[0].core, node[1].id.pk);
    step(PCORE_KEPT_WRITE_MS);
    send_text(0, 1, 1, "w");
    flush();                                    /* a write at this instant */
    now += 100;
    send_text(0, 1, 1, "x1");
    flush();                                    /* held: inside the cap, and no tick */
    check("flush: (x1 is held inside the cap)", node[1].delivered == 1 && !inbox_has(1, 1, "x1"));
    now += 160;                                 /* past the cap; still no tick */
    send_text(0, 1, 1, "x2");
    flush();
    check("flush: the write x2 causes delivers x1 as well, before any tick, in order",
          node[1].delivered == 3 && ({
              static struct pcore_dgram out[PCORE_INBOX];

              pcore_inbox(node[1].core, 1, 0, out, PCORE_INBOX) == 3 && out[1].data[1] == '1' &&
              out[2].data[1] == '2' && out[1].id == 2 && out[2].id == 3;
          }));
    pcore_tick(node[1].core, now);
    check("flush: and the tick after it has nothing left to do",
          node[1].delivered == 3 && !pcore_kept_write_pending(node[1].core));

    /* Keeping someone else writes the list: that too releases what it covers. */
    now += 100;
    send_text(0, 1, 1, "y");
    flush();
    check("flush: (y is held)", !inbox_has(1, 1, "y"));
    check("flush: keeping another peer writes the list, and y goes with it",
          pcore_keep(node[1].core, node[2].id.pk) == PCORE_OK && inbox_has(1, 1, "y"));
    now += 100;
    send_text(0, 1, 1, "z");
    flush();
    check("flush: so does releasing another peer",
          !inbox_has(1, 1, "z") && pcore_release(node[1].core, node[2].id.pk) == PCORE_OK &&
          inbox_has(1, 1, "z"));
    now += PCORE_KEPT_WRITE_MS;
    send_text(0, 1, 1, "written at once");      /* past the cap: a write at this instant */
    flush();
    now += 100;
    send_text(0, 1, 1, "last");                 /* inside the cap again: held */
    flush();
    check("flush: and releasing the sender itself - there is nothing left to wait for",
          !inbox_has(1, 1, "last") && pcore_release(node[1].core, node[0].id.pk) == PCORE_OK &&
          inbox_has(1, 1, "last"));
}

/* Keeping a node already heard: what was accepted from it before is the
 * floor at once, and what is queued from it is protected at once. */
static void test_keep_takes_what_was_heard(void)
{
    struct pkt invite;
    struct pcore_node_info info;
    uint64_t accepted;
    char text[16];
    int i;
    int k;

    world();
    meet(0, 1);
    send_text(0, 1, 1, "invitation");
    invite = q[0];
    flush();
    check("keep: (an invitation arrived before anyone was kept)", inbox_has(1, 1, "invitation"));
    pcore_keep(node[1].core, node[0].id.pk);
    check("keep: the record starts with the floor of what was already accepted",
          node[1].disk[0].epoch == node[0].epoch && node[1].disk[0].mark >= pw_get64(invite.b + 50));
    boot(1);
    accepted = cnt(1, PCORE_C_ACCEPTED_DATA);
    input(1, node[0].addr, invite.b, invite.n);
    check("keep: so the invitation cannot be replayed after a restart",
          cnt(1, PCORE_C_ACCEPTED_DATA) == accepted && inbox_n(1, 1, 0) == 0);

    /* Proved: only above a floor. */
    world();
    meet(0, 1);
    pcore_keep(node[1].core, node[0].id.pk);
    pcore_keep(node[0].core, node[1].id.pk);
    check("proved: a kept peer never heard from under seal is not proved yet",
          node_info(1, 0, &info) && !info.proved);
    send_text(0, 1, 1, "a");
    flush();
    step(PCORE_KEPT_WRITE_MS);
    send_text(0, 1, 1, "b");
    flush();
    step(PCORE_KEPT_WRITE_MS);
    check("proved: a floor made in this run proves nothing - it could have been set by a recording",
          node_info(1, 0, &info) && !info.proved && inbox_has(1, 1, "b"));
    boot(1);
    check("proved: after a restart the kept peer is not proved until it is heard",
          node_info(1, 0, &info) && !info.proved);
    send_text(0, 1, 1, "c");
    flush();
    step(PCORE_KEPT_WRITE_MS);
    check("proved: its datagram above the floor that was on disk is live - not a recording",
          inbox_has(1, 1, "c") && node_info(1, 0, &info) && info.proved);
    /* A record made for a key never heard has no floor: the first datagram
     * sets it and proves nothing; the next one does. */
    world();
    pcore_keep(node[1].core, node[0].id.pk);
    pcore_keep(node[0].core, node[1].id.pk);
    send_text(0, 1, 1, "x");            /* no route yet: starts a lookup */
    flush();
    send_text(0, 1, 1, "one");
    flush();
    step(PCORE_KEPT_WRITE_MS);
    check("proved: with no floor yet, the first sealed datagram sets the floor",
          inbox_has(1, 1, "one") && node[1].disk[0].epoch != 0);

    /* What is queued from a node is a kept peer's from the moment it is kept. */
    world();
    for (i = 0; i <= 5; i++) {
        if (i != 1) {
            meet(1, i);
        }
    }
    send_text(0, 1, 1, "invite");
    flush();
    pcore_keep(node[1].core, node[0].id.pk);
    for (k = 2; k <= 5; k++) {
        for (i = 0; i < 8; i++) {
            snprintf(text, sizeof(text), "s%d-%d", k, i);
            send_text(k, 1, 1, text);
        }
    }
    flush();
    send_text(5, 1, 1, "one more");
    send_text(4, 1, 1, "and another");
    flush();
    check("keep: an invitation that was already waiting is not pushed out by other senders",
          inbox_n(1, 1, 0) == PCORE_INBOX && inbox_has(1, 1, "invite"));
}

/* An older announcement that was missed, arriving late, changes nothing shown (review S4). */
static void test_late_beacon(void)
{
    struct pcore_node_info info;
    struct pkt old;
    uint32_t tag;
    int at;

    world();
    meet(0, 1);
    pcore_announce(node[0].core, 1, 1, 1, 2, &tag);
    pcore_set_name(node[0].core, "old name");
    now += PCORE_BEACON_MS;
    pcore_tick(node[0].core, now);
    at = find(0, PW_BEACON);
    old = q[at];
    drop_all();                                 /* missed */
    pcore_withdraw(node[0].core);
    pcore_set_name(node[0].core, "new name");
    step(PCORE_BEACON_MS);
    check("late beacon: (the newer one was heard: no table, the new name)",
          node_info(1, 0, &info) && !info.table && strcmp(info.name, "new name") == 0);
    {
        int64_t heard = info.heard_ms;

        now += 500;
        input(1, node[0].addr, old.b, old.n);
        check("late beacon: the older one, arriving late, brings back neither its table nor its name",
              cnt(1, PCORE_C_ACCEPTED_SIGNED) >= 3 && node_info(1, 0, &info) && !info.table &&
              strcmp(info.name, "new name") == 0 && info.heard_ms == heard);
    }
}

/* Frames forged in one known key's name cannot spend what the others need (review S7). */
static void test_forged_in_a_known_name(void)
{
    uint8_t frame[PW_SIGNED_SIZE];
    uint64_t accepted;
    int at;
    int i;

    world();
    meet(0, 1);
    meet(0, 2);
    now += 5000;
    visible(0);
    for (i = 0; i < 400; i++) {
        struct pw_signed f;

        memset(&f, 0, sizeof(f));
        f.h.type = PW_BEACON;
        memcpy(f.h.sender, node[1].id.pk, PC_KEY);
        f.h.epoch = node[1].epoch;
        f.h.seq = 1000000 + (uint64_t)i;        /* ahead of anything genuine: passes the precheck */
        f.addr = node[1].addr;
        f.port = PORT;
        pw_build_signed(&f, frame);
        memset(frame + PW_SIGNED_BODY, 0x42, PW_SIG);
        input(0, node[1].addr, frame, sizeof(frame));
    }
    check("forged name: of four hundred forgeries in one key's name, fifty signatures are checked",
          cnt(0, PCORE_C_AUTH) == 50 && cnt(0, PCORE_C_BUDGET) == 350);
    pcore_tick(node[2].core, now + PCORE_BEACON_MS);
    at = find(2, PW_BEACON);
    accepted = cnt(0, PCORE_C_ACCEPTED_SIGNED);
    if (at >= 0) {
        input(0, node[2].addr, q[at].b, q[at].n);
    }
    drop_all();
    check("forged name: and another known node's announcement is still heard in that instant",
          at >= 0 && cnt(0, PCORE_C_ACCEPTED_SIGNED) == accepted + 1);
    {
        struct pcore_node_info info;

        check("forged name: the forgeries moved nothing of the node they named",
              node_info(0, 1, &info) && info.addr == node[1].addr && strcmp(info.name, "node1") == 0);
    }
}

/* ---- keys that are not peers; our own key ------------------------------------------------------- */

static void forge_signed(uint8_t *buf, uint8_t type, const uint8_t key[PC_KEY], uint32_t addr)
{
    struct pw_signed f;

    memset(&f, 0, sizeof(f));
    f.h.type = type;
    memcpy(f.h.sender, key, PC_KEY);
    f.h.epoch = 3;
    f.h.seq = 9;
    f.addr = addr;
    f.port = PORT;
    f.name_len = 4;
    memcpy(f.name, "evil", 4);
    pw_build_signed(&f, buf);
    memset(buf + PW_SIGNED_BODY, 0, PW_SIG);
    buf[PW_SIGNED_BODY] = 1;
}

static void test_bad_keys(void)
{
    static const char *const bad[] = {
        "0100000000000000000000000000000000000000000000000000000000000000",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "0000000000000000000000000000000000000000000000000000000000000000",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a",
        "3b5b475c4b82dd1572799fc546f4c6c03e478c6654aa4c7f945b347ea32af60d",
        "0200000000000000000000000000000000000000000000000000000000000000",
    };
    uint8_t frame[PW_SIGNED_SIZE];
    uint8_t key[PC_KEY];
    size_t i;
    size_t k;

    world();
    visible(0);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        for (k = 0; k < PC_KEY; k++) {
            unsigned v = 0;

            sscanf(bad[i] + 2 * k, "%2x", &v);
            key[k] = (uint8_t)v;
        }
        now += 1000;
        forge_signed(frame, PW_BEACON, key, NET | 99);
        input(0, NET | 99, frame, sizeof(frame));
    }
    check("bad keys: five small-order keys, a mixed-order one and a non-point never become nodes",
          node_count(0) == 0 && cnt(0, PCORE_C_BAD_KEY) == 7 && cnt(0, PCORE_C_ACCEPTED_SIGNED) == 0);

    /* Our own key as the sender, in every type. */
    {
        uint64_t before = cnt(0, PCORE_C_OWN);

        for (i = PW_BEACON; i <= PW_LOOKUP_REPLY; i++) {
            if (i == PW_DATA) {
                continue;
            }
            forge_signed(frame, (uint8_t)i, node[0].id.pk, NET | 99);
            input(0, NET | 99, frame, sizeof(frame));
        }
        check("own key: a frame bearing this device's own key is dropped before anything else",
              cnt(0, PCORE_C_OWN) == before + 5 && node_count(0) == 0);
    }
    /* A stranger's key with a bad signature: verified within the budget, and refused. */
    forge_signed(frame, PW_BEACON, node[1].id.pk, NET | 99);
    input(0, NET | 99, frame, sizeof(frame));
    check("forged beacon: a genuine key with a signature that is not its own is refused",
          cnt(0, PCORE_C_AUTH) == 1 && node_count(0) == 0);
}

static void test_unknown_and_invisible(void)
{
    struct pkt p;
    uint64_t auth;

    world();
    meet(0, 1);
    /* Node 2 has heard node 0; node 0 has never heard node 2. */
    visible(2);
    now += 0;
    pcore_tick(node[0].core, now + PCORE_BEACON_MS);
    now += PCORE_BEACON_MS;
    {
        int at = find(0, PW_BEACON);

        if (at >= 0) {
            input(2, node[0].addr, q[at].b, q[at].n);
        }
        drop_all();
    }
    check("unknown: (node 2 knows node 0, node 0 does not know node 2)",
          node_info(2, 0, NULL) && !node_info(0, 2, NULL));
    p = capture(2, 0, "from a stranger");
    auth = cnt(0, PCORE_C_AUTH) + cnt(0, PCORE_C_BAD_KEY);
    input(0, node[2].addr, p.b, p.n);
    check("unknown sender: a sealed datagram from a key never heard is dropped unopened",
          cnt(0, PCORE_C_UNKNOWN) == 1 && cnt(0, PCORE_C_AUTH) + cnt(0, PCORE_C_BAD_KEY) == auth &&
          inbox_n(0, 1, 0) == 0);

    /* Not visible: a node that is not kept is not listened to at all. */
    p = capture(1, 0, "while you were hidden");
    pcore_set_visible(node[0].core, 0, now);
    input(0, node[1].addr, p.b, p.n);
    check("not visible: a datagram from a node that is not kept is dropped",
          cnt(0, PCORE_C_NOT_VISIBLE) >= 1 && inbox_n(0, 1, 0) == 0);
    {
        uint64_t nv = cnt(0, PCORE_C_NOT_VISIBLE);
        uint64_t tx = cnt(0, PCORE_C_TX);
        int nodes = node_count(0);

        visible(3);
        step(0);                /* node 3's beacon */
        pcore_probe(node[3].core, node[0].addr, PORT, now);
        step(0);                /* node 3's probe of node 0 */
        check("not visible: a stranger's beacon and probe make no node and draw no answer",
              cnt(0, PCORE_C_NOT_VISIBLE) >= nv + 2 && node_count(0) == nodes && cnt(0, PCORE_C_TX) == tx);
    }
    visible(0);
    input(0, node[1].addr, p.b, p.n);
    check("not visible: (visible again, the same datagram is accepted)",
          inbox_has(0, 1, "while you were hidden"));
}

/* ---- probe and lookup: liveness by challenge -------------------------------------------------------- */

static void test_probe(void)
{
    struct pcore_probe_info info[PCORE_PROBES];
    struct pcore_node_info ni;
    struct pkt reply;
    int id;
    int at;

    world();
    check("probe: not visible, none is sent", pcore_probe(node[0].core, node[1].addr, PORT, now) == PCORE_E_CLOSED);
    visible(0);
    visible(1);
    drop_all();
    tick_all();
    drop_all();                 /* no beacons: discovery is "blocked" */
    check("probe: (neither has heard the other)", !node_info(0, 1, NULL) && !node_info(1, 0, NULL));
    id = pcore_probe(node[0].core, node[1].addr, PORT, now);
    check("probe: asked for by address", id > 0);
    check("probe: a bad address is refused",
          pcore_probe(node[0].core, 0, PORT, now) == PCORE_E_PARAM &&
          pcore_probe(node[0].core, 0xFFFFFFFFu, PORT, now) == PCORE_E_PARAM &&
          pcore_probe(node[0].core, 0xE0000001u, PORT, now) == PCORE_E_PARAM &&
          pcore_probe(node[0].core, node[0].addr, PORT, now) == PCORE_E_PARAM &&
          pcore_probe(node[0].core, BCAST, PORT, now) == PCORE_E_PARAM &&
          pcore_probe(node[0].core, node[1].addr, 0, now) == PCORE_E_PARAM);
    tick_all();
    at = find(0, PW_PROBE);
    check("probe: one signed frame, unicast, the size of any signed frame",
          at >= 0 && !q[at].bcast && q[at].dst == node[1].addr && q[at].n == PW_SIGNED_SIZE);
    {
        struct pkt probe = q[at];

        remove_at(at);
        deliver(&probe);
    }
    at = find(1, PW_PROBE_REPLY);
    check("probe: answered with one frame no larger than the question",
          at >= 0 && q[at].n == PW_SIGNED_SIZE && q[at].dst == node[0].addr);
    check("probe: to the one asked, the asker is now a node, though not proved live",
          node_info(1, 0, &ni) && ni.proved == 0);
    reply = q[at];
    remove_at(at);

    /* The reply from another address: not the address that was asked. */
    input(0, NET | 77, reply.b, reply.n);
    check("probe: its answer arriving from another address is not taken",
          pcore_probes(node[0].core, info, PCORE_PROBES) == 1 && info[0].state == PCORE_PROBE_PENDING &&
          !node_info(0, 1, NULL) && cnt(0, PCORE_C_STATED) == 1);
    /* The reply with another challenge: the signature no longer covers it. */
    {
        struct pkt alt = reply;

        alt.b[98] ^= 0x01;
        input(0, node[1].addr, alt.b, alt.n);
        check("probe: an answer whose challenge was altered does not verify",
              pcore_probes(node[0].core, info, PCORE_PROBES) == 1 && info[0].state == PCORE_PROBE_PENDING &&
              cnt(0, PCORE_C_AUTH) == 1);
    }
    input(0, node[1].addr, reply.b, reply.n);
    check("probe: the genuine answer, from the address asked, settles it",
          pcore_probes(node[0].core, info, PCORE_PROBES) == 1 && info[0].state == PCORE_PROBE_OK &&
          memcmp(info[0].key, node[1].id.pk, PC_KEY) == 0 && (int)info[0].id == id);
    check("probe: and the node it found is proved live, with its address",
          node_info(0, 1, &ni) && ni.proved == 1 && ni.addr == node[1].addr);
    check("probe: so a datagram can go, with no beacon ever heard",
          send_text(0, 1, 1, "found you") == PCORE_OK && (flush(), 1) && inbox_has(1, 1, "found you"));

    /* An old answer does not settle a new probe. */
    drop_all();
    now += 5000;
    id = pcore_probe(node[0].core, node[1].addr, PORT, now);
    input(0, node[1].addr, reply.b, reply.n);
    check("probe: an earlier answer, replayed, does not settle a later probe", ({
              int n = pcore_probes(node[0].core, info, PCORE_PROBES);
              int k;
              int pending = 0;

              for (k = 0; k < n; k++) {
                  pending |= (int)info[k].id == id && info[k].state == PCORE_PROBE_PENDING;
              }
              pending;
          }));

    /* Nobody there. */
    world();
    visible(0);
    drop_all();
    id = pcore_probe(node[0].core, NET | 200, PORT, now);
    {
        int sent = 0;
        int i;
        int k;

        for (i = 0; i < 20; i++) {
            now += 250;
            visible(0);
            pcore_tick(node[0].core, now);
            for (k = 0; k < qn; k++) {
                sent += q[k].b[1] == PW_PROBE && q[k].dst == (NET | 200) && !q[k].bcast;
            }
            drop_all();
        }
        check("probe: nobody there - three tries, a second apart, then no answer",
              sent == PCORE_PROBE_TRIES && pcore_probes(node[0].core, info, PCORE_PROBES) == 1 &&
              info[0].state == PCORE_PROBE_FAILED);
    }
    /* A probe does not outlive the lease: it cannot hold the socket open. */
    visible(0);
    id = pcore_probe(node[0].core, NET | 201, PORT, now);
    pcore_tick(node[0].core, now);
    drop_all();
    pcore_set_visible(node[0].core, 0, now);
    pcore_tick(node[0].core, now);
    check("probe: when visibility ends a pending probe ends with it, and the socket is not wanted", ({
              int n = pcore_probes(node[0].core, info, PCORE_PROBES);
              int k;
              int ended = 0;

              for (k = 0; k < n; k++) {
                  ended |= (int)info[k].id == id && info[k].state == PCORE_PROBE_FAILED;
              }
              ended && qn == 0 && !pcore_wants_socket(node[0].core, now);
          }));
    visible(0);
    {
        int i;
        int last = 0;

        for (i = 0; i < PCORE_PROBES; i++) {
            last = pcore_probe(node[0].core, NET | (uint32_t)(201 + i), PORT, now);
        }
        check("probe: four at once is the most", last > 0 &&
              pcore_probe(node[0].core, NET | 250, PORT, now) == PCORE_E_FULL);
    }
}

static void test_reply_limits(void)
{
    uint64_t tx;
    int i;

    world();
    visible(0);
    visible(1);
    drop_all();
    tick_all();
    drop_all();
    tx = cnt(1, PCORE_C_TX);
    /* The same asker, ten probes in one moment. */
    for (i = 0; i < 10; i++) {
        pcore_probe(node[0].core, node[1].addr, PORT, now);
        pcore_tick(node[0].core, now);
        flush();
        {
            struct pcore_probe_info info[PCORE_PROBES];

            (void)pcore_probes(node[0].core, info, PCORE_PROBES);
        }
        now += 1;
    }
    check("replies: ten questions from one node in a moment draw one answer",
          cnt(1, PCORE_C_TX) - tx == 1 && cnt(1, PCORE_C_REPLY_LIMITED) >= 1);
}

static void test_stated_address(void)
{
    struct pcore_node_info info;
    struct pkt b;
    int at;

    world();
    visible(0);
    visible(1);
    tick_all();
    at = find(1, PW_BEACON);
    b = q[at];
    drop_all();
    /* A stranger's beacon arriving from an address it does not state. */
    input(0, NET | 66, b.b, b.n);
    check("stated address: a stranger's beacon from another address than it states is dropped",
          cnt(0, PCORE_C_STATED) == 1 && !node_info(0, 1, NULL));
    input(0, node[1].addr, b.b, b.n);
    check("stated address: from the address it states, it makes a node",
          node_info(0, 1, &info) && info.addr == node[1].addr);
    /* A known node's later beacon from elsewhere: heard, but the endpoint stays. */
    now += PCORE_BEACON_MS;
    pcore_tick(node[1].core, now);
    at = find(1, PW_BEACON);
    b = q[at];
    drop_all();
    input(0, NET | 66, b.b, b.n);
    check("stated address: a known node's beacon from elsewhere is recorded as seen, and changes nothing",
          cnt(0, PCORE_C_ACCEPTED_SIGNED) == 2 && node_info(0, 1, &info) && info.addr == node[1].addr &&
          info.heard_ms == now - PCORE_BEACON_MS);
    input(0, node[1].addr, b.b, b.n);
    check("stated address: and that copy is spent - from the right address too, it is a replay",
          cnt(0, PCORE_C_ACCEPTED_SIGNED) == 2 && cnt(0, PCORE_C_REPLAY) == 1);
}

static void test_endpoint_follows_key(void)
{
    struct pcore_node_info info;
    struct pkt missed;

    world();
    meet(0, 1);
    pcore_keep(node[0].core, node[1].id.pk);
    pcore_keep(node[1].core, node[0].id.pk);
    missed = capture(1, 0, "missed, from the old address");
    send_text(1, 0, 1, "second");
    flush();
    /* Node 1 gets a new address (a new lease, a new MAC) and says nothing. */
    node[1].addr = NET | 88;
    {
        struct pcore_iface ifc = { node[1].addr, BCAST };

        pcore_set_ifaces(node[1].core, &ifc, 1);
    }
    pcore_set_visible(node[0].core, 0, now);
    pcore_set_visible(node[1].core, 0, now);
    check("endpoint: (the peer is still known at its old address)",
          node_info(0, 1, &info) && info.addr == (NET | 11));
    send_text(1, 0, 1, "from the new address");
    flush();
    step(PCORE_KEPT_WRITE_MS);
    check("endpoint: its next datagram, from a new address, moves the endpoint with the key",
          inbox_has(0, 1, "from the new address") && node_info(0, 1, &info) && info.addr == (NET | 88));
    check("endpoint: and the kept record on disk has the new address",
          node[0].disk[0].addr == (NET | 88));
    check("endpoint: so the answer goes to the new address",
          send_text(0, 1, 1, "to the new one") == PCORE_OK && q[0].dst == (NET | 88) && (flush(), 1) &&
          inbox_has(1, 1, "to the new one"));
    /* A frame that was missed, inside the window, arriving from somewhere
     * else entirely: accepted as a datagram, and it moves nothing. */
    input(0, NET | 66, missed.b, missed.n);
    step(PCORE_KEPT_WRITE_MS);
    check("endpoint: a missed frame replayed from elsewhere is delivered but moves nothing",
          inbox_has(0, 1, "missed, from the old address") && node_info(0, 1, &info) &&
          info.addr == (NET | 88));
    /* Addresses that can never be an endpoint. */
    send_text(1, 0, 1, "from broadcast");
    {
        struct pkt p = q[0];

        drop_all();
        input(0, 0xFFFFFFFFu, p.b, p.n);
        step(PCORE_KEPT_WRITE_MS);
        check("endpoint: a broadcast source address is never one",
              node_info(0, 1, &info) && info.addr == (NET | 88));
    }
}

static void test_lookup(void)
{
    struct pcore_node_info info;
    uint64_t tx2;

    world();
    meet(0, 1);
    visible(2);
    step(0);
    pcore_keep(node[0].core, node[1].id.pk);
    pcore_keep(node[1].core, node[0].id.pk);
    send_text(0, 1, 1, "before");
    flush();
    step(PCORE_KEPT_WRITE_MS);
    /* Everyone goes quiet; node 1 moves; a long time passes. */
    pcore_set_visible(node[0].core, 0, now);
    pcore_set_visible(node[1].core, 0, now);
    pcore_set_visible(node[2].core, 0, now);
    node[1].addr = NET | 90;
    {
        struct pcore_iface ifc = { node[1].addr, BCAST };

        pcore_set_ifaces(node[1].core, &ifc, 1);
    }
    step(PCORE_STALE_MS + 1000);
    drop_all();
    tx2 = cnt(2, PCORE_C_TX);
    check("lookup: a send to a kept peer not heard for a while is tried where it was, and it is asked for",
          send_text(0, 1, 1, "are you there") == PCORE_OK && find(0, PW_LOOKUP) >= 0 && find(0, PW_DATA) >= 0);
    check("lookup: the question goes to its last address and to the link", ({
              int unicast = 0;
              int bcast = 0;
              int i;

              for (i = 0; i < qn; i++) {
                  if (q[i].b[1] == PW_LOOKUP) {
                      unicast += !q[i].bcast && q[i].dst == (NET | 11);
                      bcast += q[i].bcast;
                  }
              }
              unicast == 1 && bcast == 1;
          }));
    flush();
    check("lookup: the peer itself answers, though it is not visible - it keeps the asker",
          node_info(0, 1, &info) && info.addr == (NET | 90));
    check("lookup: nobody else answers a question about another key",
          cnt(2, PCORE_C_TX) == tx2 && cnt(2, PCORE_C_NOT_VISIBLE) >= 1);
    check("lookup: and the next datagram reaches it at its new address",
          send_text(0, 1, 1, "found") == PCORE_OK && (flush(), 1) && inbox_has(1, 1, "found"));
    check("lookup: it is not asked again while it answers",
          (send_text(0, 1, 1, "again"), find(0, PW_LOOKUP) < 0));
    flush();

    /* A kept peer with no address at all: nothing can go yet, and it is asked for. */
    world();
    pcore_keep(node[0].core, node[1].id.pk);
    pcore_keep(node[1].core, node[0].id.pk);
    check("lookup: a kept peer never heard has no route yet, and is looked for",
          send_text(0, 1, 1, "x") == PCORE_E_NO_ROUTE && find(0, PW_LOOKUP) >= 0 && find(0, PW_DATA) < 0);
    flush();
    check("lookup: found by the question alone, on a network where neither is visible",
          node_info(0, 1, &info) && info.addr == node[1].addr &&
          send_text(0, 1, 1, "now") == PCORE_OK && (flush(), 1) && inbox_has(1, 1, "now"));
    {
        uint64_t before = cnt(0, PCORE_C_TX);

        world();
        pcore_keep(node[0].core, node[1].id.pk);
        before = cnt(0, PCORE_C_TX);
        send_text(0, 1, 1, "x");
        send_text(0, 1, 1, "x");
        send_text(0, 1, 1, "x");
        check("lookup: asked at most once in five seconds, however often a send is tried",
              cnt(0, PCORE_C_TX) - before == 1);
        now += PCORE_LOOKUP_INTERVAL_MS;
        send_text(0, 1, 1, "x");
        check("lookup: and again after them", cnt(0, PCORE_C_TX) - before == 2);
    }
}

/* ---- exhaustion: nodes, the inbox, the verification budget --------------------------------------------- */

/* A beacon from a brand-new identity at a given address, genuinely signed. */
static void stranger_beacon(int to, uint32_t addr)
{
    static const struct pcore_ops none = { op_send, op_source_for, op_kept_save, NULL };
    struct sim tmp;
    struct pcore_iface ifc;
    int before = qn;

    memset(&tmp, 0, sizeof(tmp));
    pc_identity_generate(&tmp.id);
    tmp.addr = addr;
    /* The sim's op_send needs an index: borrow slot N-1's place in the array. */
    {
        struct sim saved = node[N - 1];

        node[N - 1] = tmp;
        node[N - 1].core = pcore_new(&node[N - 1].id, 1, "stranger", PORT, &none, &node[N - 1], NULL, 0);
        ifc.addr = addr;
        ifc.bcast = BCAST;
        pcore_set_ifaces(node[N - 1].core, &ifc, 1);
        pcore_set_visible(node[N - 1].core, 1, now);
        pcore_tick(node[N - 1].core, now);
        if (qn > before) {
            input(to, addr, q[before].b, q[before].n);
            qn = before;
        }
        pcore_free(node[N - 1].core);
        pc_identity_wipe(&node[N - 1].id);
        node[N - 1] = saved;
    }
}

static void test_node_table(void)
{
    struct pcore_node_info info;
    int i;

    world();
    meet(0, 1);
    pcore_keep(node[0].core, node[1].id.pk);
    pcore_keep(node[1].core, node[0].id.pk);
    for (i = 0; i < 60; i++) {
        now += 100;             /* within the budget for unknown keys */
        visible(0);
        stranger_beacon(0, NET | (uint32_t)(100 + i));
    }
    check("node table: sixty invented players fill the 28 places strangers may have, and no more",
          node_count(0) == PCORE_NODES - PCORE_KEPT + 1);
    check("node table: the least recently heard were replaced", cnt(0, PCORE_C_EVICTED_NODES) == 32);
    check("node table: the kept peer is still in it", node_info(0, 1, &info) && info.kept);
    check("node table: and its datagram is still accepted",
          send_text(1, 0, 1, "still here") == PCORE_OK && (flush(), 1) && (step(PCORE_KEPT_WRITE_MS), 1) &&
          inbox_has(0, 1, "still here"));
    pcore_keep(node[0].core, node[2].id.pk);
    pcore_keep(node[0].core, node[3].id.pk);
    pcore_keep(node[0].core, node[4].id.pk);
    check("node table: full of strangers, the four places kept peers have are still there",
          node_count(0) == PCORE_NODES && node_info(0, 4, &info) && info.kept &&
          cnt(0, PCORE_C_EVICTED_NODES) == 32);
}

static void test_verify_budget(void)
{
    uint64_t accepted;
    int at;
    int i;

    world();
    meet(0, 1);
    now += 5000;                /* both budgets full */
    visible(0);
    accepted = cnt(0, PCORE_C_ACCEPTED_SIGNED);
    for (i = 0; i < 100; i++) {
        stranger_beacon(0, NET | (uint32_t)(100 + i % 100));
    }
    check("budget: a hundred invented keys in one instant - twenty signatures are checked, no more",
          cnt(0, PCORE_C_ACCEPTED_SIGNED) - accepted == 20 && cnt(0, PCORE_C_BUDGET) == 80);
    /* A known node's beacon in that same instant has its own budget. */
    pcore_tick(node[1].core, now + PCORE_BEACON_MS);
    at = find(1, PW_BEACON);
    accepted = cnt(0, PCORE_C_ACCEPTED_SIGNED);
    if (at >= 0) {
        input(0, node[1].addr, q[at].b, q[at].n);
    }
    drop_all();
    check("budget: and a known peer's beacon is still heard in the middle of that flood",
          cnt(0, PCORE_C_ACCEPTED_SIGNED) == accepted + 1);
}

static void test_inbox(void)
{
    static struct pcore_dgram out[PCORE_INBOX];
    char text[16];
    int i;
    int k;

    world();
    for (i = 1; i <= 5; i++) {
        meet(0, i);
    }
    /* One sender: at most eight of its datagrams are held. */
    for (i = 0; i < 12; i++) {
        snprintf(text, sizeof(text), "s1-%02d", i);
        send_text(1, 0, 1, text);
    }
    flush();
    check("inbox: of twelve from one sender the newest eight are held",
          pcore_inbox(node[0].core, 1, 0, out, PCORE_INBOX) == PCORE_INBOX_PER_SENDER &&
          memcmp(out[0].data, "s1-04", 5) == 0 && memcmp(out[7].data, "s1-11", 5) == 0);
    /* Fill it: four senders, eight each. */
    for (k = 2; k <= 4; k++) {
        for (i = 0; i < 8; i++) {
            snprintf(text, sizeof(text), "s%d-%02d", k, i);
            send_text(k, 0, 1, text);
        }
    }
    flush();
    check("inbox: four senders at their caps fill it", inbox_n(0, 1, 0) == PCORE_INBOX);
    /* A fifth sender, not kept: the oldest that is not a kept peer's goes. */
    send_text(5, 0, 1, "s5-new");
    flush();
    check("inbox: full, a newcomer pushes out the oldest datagram",
          inbox_n(0, 1, 0) == PCORE_INBOX && inbox_has(0, 1, "s5-new") && !inbox_has(0, 1, "s1-04"));

    /* Kept peers' datagrams are never pushed out by anyone else's. */
    world();
    for (i = 1; i <= 5; i++) {
        meet(0, i);
    }
    for (k = 1; k <= 4; k++) {
        pcore_keep(node[0].core, node[k].id.pk);
        pcore_keep(node[k].core, node[0].id.pk);
    }
    for (k = 1; k <= 4; k++) {
        for (i = 0; i < 8; i++) {
            snprintf(text, sizeof(text), "k%d-%02d", k, i);
            send_text(k, 0, 1, text);
            flush();
            step(PCORE_KEPT_WRITE_MS);
        }
    }
    check("inbox: four kept peers at their caps fill it", inbox_n(0, 1, 0) == PCORE_INBOX);
    send_text(5, 0, 1, "stranger");
    flush();
    check("inbox: full of kept peers' datagrams, a stranger's does not get in",
          inbox_n(0, 1, 0) == PCORE_INBOX && !inbox_has(0, 1, "stranger") &&
          cnt(0, PCORE_C_INBOX_FULL) == 1 && inbox_has(0, 1, "k1-00"));
    send_text(2, 0, 1, "k2-new");
    flush();
    step(PCORE_KEPT_WRITE_MS);
    check("inbox: a kept peer's next one replaces that peer's own oldest, nobody else's",
          inbox_has(0, 1, "k2-new") && !inbox_has(0, 1, "k2-00") && inbox_has(0, 1, "k1-00") &&
          inbox_has(0, 1, "k3-00"));

    /* A kept peer arriving at an inbox full of strangers' datagrams gets in. */
    world();
    for (i = 1; i <= 5; i++) {
        meet(0, i);
    }
    for (k = 2; k <= 5; k++) {
        for (i = 0; i < 8; i++) {
            snprintf(text, sizeof(text), "x%d-%02d", k, i);
            send_text(k, 0, 1, text);
        }
    }
    flush();
    pcore_keep(node[0].core, node[1].id.pk);
    pcore_keep(node[1].core, node[0].id.pk);
    send_text(1, 0, 1, "the move that matters");
    flush();
    step(PCORE_KEPT_WRITE_MS);
    check("inbox: full of strangers' datagrams, a kept peer's still gets in",
          inbox_n(0, 1, 0) == PCORE_INBOX && inbox_has(0, 1, "the move that matters") &&
          !inbox_has(0, 1, "x2-00"));
}

/* ---- the end of the counters ------------------------------------------------------------------------- */

static void test_sequence_limit(void)
{
    uint64_t tx;

    world();
    meet(0, 1);
    pcore_test_set_sequence(node[0].core, UINT64_MAX - 1);
    check("limit: the last sequence there is can still be used",
          send_text(0, 1, 1, "last") == PCORE_OK && pcore_sequence(node[0].core) == UINT64_MAX);
    flush();
    tx = cnt(0, PCORE_C_TX);
    check("limit: after it nothing is sealed and nothing wraps",
          send_text(0, 1, 1, "one too many") == PCORE_E_SEND && pcore_sequence(node[0].core) == UINT64_MAX &&
          qn == 0);
    step(PCORE_BEACON_MS);
    step(PCORE_BEACON_MS);
    check("limit: nor is anything announced", cnt(0, PCORE_C_TX) == tx);
    check("limit: (the last one did arrive)", inbox_has(1, 1, "last") && !inbox_has(1, 1, "one too many"));
}

static void test_names_and_misc(void)
{
    struct pcore_node_info info;
    int i;

    world();
    check("name: set to clean text", pcore_set_name(node[0].core, "Bj\xc3\xb8rn") == PCORE_OK &&
          strcmp(pcore_name(node[0].core), "Bj\xc3\xb8rn") == 0);
    check("name: a control character or 25 bytes is refused, and the name stays",
          pcore_set_name(node[0].core, "a\tb") == PCORE_E_PARAM &&
          pcore_set_name(node[0].core, "1234567890123456789012345") == PCORE_E_PARAM &&
          strcmp(pcore_name(node[0].core), "Bj\xc3\xb8rn") == 0);
    meet(0, 1);
    check("name: it is what the other node sees", node_info(1, 0, &info) &&
          strcmp(info.name, "Bj\xc3\xb8rn") == 0);
    check("counters: every one has a name", ({
              int ok = 1;

              for (i = 0; i < PCORE_C_COUNT; i++) {
                  ok &= pcore_counter_name((enum pcore_count)i)[0] != '?';
              }
              ok && pcore_counter_name(PCORE_C_COUNT)[0] == '?';
          }));
    check("engine: one cannot be made from nothing",
          pcore_new(NULL, 1, "", PORT, &OPS, NULL, NULL, 0) == NULL &&
          pcore_new(&node[0].id, 0, "", PORT, &OPS, NULL, NULL, 0) == NULL &&
          pcore_new(&node[0].id, 1, "", 0, &OPS, NULL, NULL, 0) == NULL &&
          pcore_new(&node[0].id, 1, "", PORT, &OPS, NULL, NULL, 5) == NULL);
    pcore_input(NULL, 0, 0, NULL, 0, 0);
    pcore_tick(NULL, 0);
    pcore_free(NULL);
    check("engine: nothing at all is harmless", pcore_visible(NULL, 0) == 0 &&
          pcore_wants_socket(NULL, 0) == 0 && pcore_counter(NULL, PCORE_C_RX) == 0);
    {
        /* A kept list read from disk that names this device itself, or one
         * peer twice, is not taken. */
        struct ps_kept recs[3];
        struct pcore *c;

        memset(recs, 0, sizeof(recs));
        memcpy(recs[0].key, node[0].id.pk, PC_KEY);
        memcpy(recs[1].key, node[1].id.pk, PC_KEY);
        memcpy(recs[2].key, node[1].id.pk, PC_KEY);
        c = pcore_new(&node[0].id, 9, "", PORT, &OPS, &node[0], recs, 3);
        check("engine: its own key and a repeated peer in the kept list are not taken", ({
                  struct ps_kept list[PCORE_KEPT];

                  c != NULL && pcore_kept(c, list, PCORE_KEPT) == 1 &&
                  memcmp(list[0].key, node[1].id.pk, PC_KEY) == 0;
              }));
        pcore_free(c);
    }
}

/* A node that is not kept: after the receiver's restart there is no state
 * for it. ADR-017 section 6 states this residual; the test holds the code to
 * exactly what the ADR says, no more and no less. */
static void test_nonkept_residual(void)
{
    struct pcore_node_info info;
    struct pkt beacon;
    struct pkt data;
    int at;

    world();
    visible(0);
    visible(1);
    tick_all();
    at = find(0, PW_BEACON);
    beacon = q[at];
    flush();
    data = capture(0, 1, "recorded");
    input(1, node[0].addr, data.b, data.n);
    boot(1);                    /* node 1 restarts; it kept nobody */
    check("residual: after a restart a node that was not kept is not a node",
          !node_info(1, 0, NULL) && !pcore_wants_socket(node[1].core, now));
    input(1, node[0].addr, data.b, data.n);
    check("residual: so its recorded datagram is dropped - unknown, or not listened to at all",
          inbox_n(1, 1, 0) == 0 && cnt(1, PCORE_C_UNKNOWN) + cnt(1, PCORE_C_NOT_VISIBLE) == 1);
    visible(1);
    input(1, node[0].addr, beacon.b, beacon.n);
    check("residual: its recorded beacon does make a node again - a claim, not proved live",
          node_info(1, 0, &info) && info.proved == 0);
    input(1, node[0].addr, data.b, data.n);
    check("residual: and then that one recorded datagram is accepted once more, as the ADR states",
          inbox_has(1, 1, "recorded"));
    check("residual: but a recording proves nobody live", node_info(1, 0, &info) && info.proved == 0);
    input(1, node[0].addr, data.b, data.n);
    check("residual: but not twice", inbox_n(1, 1, 0) == 1);
}

int main(void)
{
    int i;

    if (pc_init() != 0) {
        printf("FAIL libsodium cannot start\n");
        return 1;
    }
    test_silence_and_discovery();
    test_datagram();
    test_forgeries();
    test_replay();
    test_lower_epoch();
    test_kept_restart();
    test_data_behind_beacon();
    test_kept_write();
    test_floor_on_disk();
    test_held_is_flushed();
    test_keep_takes_what_was_heard();
    test_late_beacon();
    test_forged_in_a_known_name();
    test_bad_keys();
    test_unknown_and_invisible();
    test_probe();
    test_reply_limits();
    test_stated_address();
    test_endpoint_follows_key();
    test_lookup();
    test_node_table();
    test_verify_budget();
    test_inbox();
    test_sequence_limit();
    test_names_and_misc();
    test_nonkept_residual();
    for (i = 0; i < N; i++) {
        pcore_free(node[i].core);
        pc_identity_wipe(&node[i].id);
    }
    printf("peer_core_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}

/*
 * pocketpcap network intelligence. See ppc_net.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ppc_net.h"

#include <stdlib.h>
#include <string.h>

#define ETH_HDR 14u
#define NET_PROBES 8u
/* DHCP: the fixed BOOTP part, the magic cookie, then options. */
#define DHCP_FIXED 236u
#define DHCP_OPTIONS (DHCP_FIXED + 4u)
#define DHCP_COOKIE 0x63825363u
#define JUDGED_OFFER 1u
#define JUDGED_ACK 2u

static const uint8_t none[6];

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t fnv(uint32_t h, const uint8_t *p, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h ^ (h >> 15);
}

static uint64_t since(uint64_t now, uint64_t then)
{
    return now >= then ? now - then : 0;
}

/* A station's own address: not a group address, not all zeros. */
static int mac_ok(const uint8_t *m)
{
    return !(m[0] & 1u) && memcmp(m, none, 6) != 0;
}

/* An address a station can hold: not 0.x, not multicast or broadcast. */
static int ip_ok(const uint8_t *ip)
{
    return ip[0] != 0 && ip[0] < 224;
}

int ppc_net_init(struct ppc_net *nt)
{
    memset(nt, 0, sizeof(*nt));
    /* One allocation for the three tables: every entry is a multiple of 8
     * bytes long, so each table after the first is aligned. */
    nt->mem = calloc(1, PPC_NET_HOSTS * sizeof(*nt->hosts) + PPC_NET_BINDS * sizeof(*nt->binds) +
                            PPC_NET_DHCP * sizeof(*nt->dhcp));
    if (!nt->mem) {
        return -1;
    }
    nt->hosts = nt->mem;
    nt->binds = (struct ppc_net_bind *)(void *)(nt->hosts + PPC_NET_HOSTS);
    nt->dhcp = (struct ppc_net_dhcp *)(void *)(nt->binds + PPC_NET_BINDS);
    return 0;
}

void ppc_net_free(struct ppc_net *nt)
{
    free(nt->mem);
    memset(nt, 0, sizeof(*nt));
}

/* ---- events ----------------------------------------------------------------------------- */

/* Whether an event of this kind is one per client or station (its MAC is
 * part of what it is about) or one per address. */
static int keyed_by_mac(unsigned kind)
{
    switch (kind) {
    case PPC_NK_DHCP_NAK:
    case PPC_NK_DHCP_DECLINE:
    case PPC_NK_DHCP_NO_OFFER:
    case PPC_NK_DHCP_NO_ACK:
    case PPC_NK_DHCP_SHORT_LEASE:
    case PPC_NK_ARP_SWEEP:
    case PPC_NK_ARP_SENDER:
        return 1;
    default:
        return 0;
    }
}

static void put(uint8_t *dst, const uint8_t *src, unsigned n)
{
    if (src) {
        memcpy(dst, src, n);
    } else {
        memset(dst, 0, n);
    }
}

/* Record an event. The same thing seen again is counted on the event that
 * is there; a new one takes a free place, else the oldest note's, else the
 * oldest warning's. */
static void event(struct ppc_net *nt, unsigned kind, unsigned sev, const uint8_t *ip, const uint8_t *mac,
                  const uint8_t *ip2, const uint8_t *mac2, uint64_t now, uint32_t seq)
{
    struct ppc_net_event *spare = NULL;
    struct ppc_net_event *e;
    uint8_t kip[4];
    uint8_t kmac[6];
    unsigned i;

    put(kip, ip, 4);
    put(kmac, mac, 6);
    for (i = 0; i < PPC_NET_EVENTS; i++) {
        e = &nt->events[i];
        if (e->used && e->kind == kind && memcmp(e->ip, kip, 4) == 0 &&
            (!keyed_by_mac(kind) || memcmp(e->mac, kmac, 6) == 0)) {
            if (e->count < 0xffffffffu) {
                e->count++;
            }
            e->last_ns = now;
            e->seq = seq;
            memcpy(e->mac, kmac, 6);
            put(e->ip2, ip2, 4);
            put(e->mac2, mac2, 6);
            return;
        }
        if (!spare || (spare->used && (!e->used || e->sev < spare->sev ||
                                       (e->sev == spare->sev && e->last_ns < spare->last_ns)))) {
            spare = e;
        }
    }
    if (spare->used) {
        nt->st.events_lost++;
    }
    memset(spare, 0, sizeof(*spare));
    spare->used = 1;
    spare->kind = (uint8_t)kind;
    spare->sev = (uint8_t)sev;
    memcpy(spare->ip, kip, 4);
    memcpy(spare->mac, kmac, 6);
    put(spare->ip2, ip2, 4);
    put(spare->mac2, mac2, 6);
    spare->count = 1;
    spare->seq = seq;
    spare->first_ns = now;
    spare->last_ns = now;
}

/* ---- devices ---------------------------------------------------------------------------- */

/* The device's entry, or NULL. With `make`, a new one when it is not there:
 * a free place among the probes, else the one silent longest. */
static struct ppc_net_host *host_of(struct ppc_net *nt, const uint8_t *mac, uint64_t now, int make)
{
    uint32_t h;
    struct ppc_net_host *spare = NULL;
    uint32_t i;

    if (!mac_ok(mac)) {
        return NULL;
    }
    h = fnv(2166136261u, mac, 6);
    for (i = 0; i < NET_PROBES; i++) {
        struct ppc_net_host *e = &nt->hosts[(h + i) & (PPC_NET_HOSTS - 1u)];

        if (e->used && memcmp(e->mac, mac, 6) == 0) {
            return e;
        }
        if (!spare || (spare->used && (!e->used || e->last_ns < spare->last_ns))) {
            spare = e;
        }
    }
    if (!make) {
        return NULL;
    }
    if (spare->used) {
        nt->st.hosts_replaced++;
    } else {
        nt->st.hosts++;
    }
    memset(spare, 0, sizeof(*spare));
    memcpy(spare->mac, mac, 6);
    spare->used = 1;
    spare->first_ns = now;
    spare->last_ns = now;
    return spare;
}

static void host_ip(struct ppc_net_host *h, const uint8_t *ip)
{
    if (h && ip_ok(ip)) {
        memcpy(h->ip, ip, 4);
        h->flags |= PPC_NH_IP;
    }
}

/* ---- ARP -------------------------------------------------------------------------------- */

static struct ppc_net_bind *bind_of(struct ppc_net *nt, const uint8_t *ip, uint64_t now, int make)
{
    uint32_t h = fnv(2166136261u, ip, 4);
    struct ppc_net_bind *spare = NULL;
    uint32_t i;

    for (i = 0; i < NET_PROBES; i++) {
        struct ppc_net_bind *b = &nt->binds[(h + i) & (PPC_NET_BINDS - 1u)];

        if (b->used && memcmp(b->ip, ip, 4) == 0) {
            return b;
        }
        /* The router's entry is the last to go. */
        if (!spare || (spare->used && (!b->used || b->router < spare->router ||
                                       (b->router == spare->router && b->last_ns < spare->last_ns)))) {
            spare = b;
        }
    }
    if (!make) {
        return NULL;
    }
    if (spare->used && spare->have && nt->st.arp_binds) {
        nt->st.arp_binds--;
    }
    memset(spare, 0, sizeof(*spare));
    memcpy(spare->ip, ip, 4);
    spare->used = 1;
    spare->last_ns = now;
    return spare;
}

static void wait_clear(struct ppc_net *nt, const uint8_t *ip)
{
    unsigned i;

    for (i = 0; i < PPC_NET_WAITS && nt->waiting; i++) {
        struct ppc_net_wait *w = &nt->waits[i];

        if (w->used && memcmp(w->ip, ip, 4) == 0) {
            w->used = 0;
            nt->waiting--;
        }
    }
}

/* This device asked who has `ip`. */
static void wait_add(struct ppc_net *nt, const uint8_t *ip, uint64_t now, uint32_t seq)
{
    struct ppc_net_wait *spare = NULL;
    unsigned i;

    for (i = 0; i < PPC_NET_WAITS; i++) {
        struct ppc_net_wait *w = &nt->waits[i];

        if (w->used && memcmp(w->ip, ip, 4) == 0) {
            if (w->tries < 0xff) {
                w->tries++;
            }
            w->last_ns = now;
            return;
        }
        if (!spare || (spare->used && (!w->used || w->last_ns < spare->last_ns))) {
            spare = w;
        }
    }
    if (!spare->used) {
        nt->waiting++;
    }
    memset(spare, 0, sizeof(*spare));
    memcpy(spare->ip, ip, 4);
    spare->used = 1;
    spare->tries = 1;
    spare->seq = seq;
    spare->last_ns = now;
}

/* A sender says `ip` is at `mac`. */
static void claim(struct ppc_net *nt, const uint8_t *ip, const uint8_t *mac, uint64_t now, uint32_t seq)
{
    struct ppc_net_bind *b = bind_of(nt, ip, now, 1);

    if (!b->have) {
        memcpy(b->mac, mac, 6);
        b->have = 1;
        nt->st.arp_binds++;
    } else if (memcmp(b->mac, mac, 6) != 0) {
        /* Another MAC than the one that held it. When that MAC is the one
         * before, and the holder spoke moments ago, two stations are using
         * the address; a single change is a hand-over. */
        if (b->have_prev && memcmp(b->prev, mac, 6) == 0 && since(now, b->last_ns) <= PPC_NET_FLIP_NS) {
            nt->st.arp_flips++;
            event(nt, PPC_NK_ARP_DUPLICATE, PPC_NS_WARN, ip, mac, NULL, b->mac, now, seq);
        } else {
            nt->st.arp_moved++;
            event(nt, b->router ? PPC_NK_ARP_ROUTER_MOVED : PPC_NK_ARP_MOVED, b->router ? PPC_NS_WARN : PPC_NS_NOTE, ip,
                  mac, NULL, b->mac, now, seq);
        }
        memcpy(b->prev, b->mac, 6);
        b->have_prev = 1;
        memcpy(b->mac, mac, 6);
    }
    if (b->probed) {
        if (since(now, b->probe_ns) > PPC_NET_PROBE_NS) {
            b->probed = 0;
        } else if (memcmp(b->probe_mac, mac, 6) != 0) {
            /* RFC 5227: the address a station probed for is in use. */
            nt->st.arp_conflicts++;
            event(nt, PPC_NK_ARP_CONFLICT, PPC_NS_WARN, ip, mac, NULL, b->probe_mac, now, seq);
            b->probed = 0;
        }
    }
    b->last_ns = now;
    b->seq = seq;
    if (b->router) {
        struct ppc_net_host *h = host_of(nt, mac, now, 0);

        if (h) {
            h->flags |= PPC_NH_ROUTER;
        }
    }
    wait_clear(nt, ip);
}

/* A station asked for one more address: many in a short while is a sweep. */
static void sweep(struct ppc_net *nt, struct ppc_net_host *h, const uint8_t *target, uint64_t now, uint32_t seq)
{
    if (since(now, h->sweep_ns) > PPC_NET_SWEEP_NS) {
        h->sweep_bits = 0;
        h->sweep_ns = now;
    }
    h->sweep_bits |= 1ull << ((target[3] ^ (unsigned)(target[2] << 3)) & 63u);
    if ((unsigned)__builtin_popcountll(h->sweep_bits) >= PPC_NET_SWEEP_BUCKETS) {
        /* A router refills its table like this all day: not worth a line. */
        if (!(h->flags & PPC_NH_ROUTER)) {
            event(nt, PPC_NK_ARP_SWEEP, PPC_NS_NOTE, (h->flags & PPC_NH_IP) ? h->ip : NULL, h->mac, NULL, NULL, now,
                  seq);
        }
        h->sweep_bits = 0;
        h->sweep_ns = now;
    }
}

static void arp(struct ppc_net *nt, const struct ppc_record *r, const uint8_t *d, struct ppc_net_host *from,
                uint32_t seq)
{
    const uint8_t *spa = r->src;
    const uint8_t *sha = r->src + 4;
    const uint8_t *tpa = r->dst;
    uint64_t now = r->rel_ns;
    int no_sender = memcmp(spa, none, 4) == 0;
    int own = memcmp(sha, d + 6, 6) == 0; /* the ARP sender is the frame's source */

    if (!mac_ok(sha)) {
        return; /* not ARP for IPv4 over Ethernet, or nothing a station sends */
    }
    if (r->a == 1) {
        nt->st.arp_requests++;
        if (no_sender) {
            struct ppc_net_bind *b;

            nt->st.arp_probes++;
            if (ip_ok(tpa)) {
                b = bind_of(nt, tpa, now, 1);
                b->probed = 1;
                memcpy(b->probe_mac, sha, 6);
                b->probe_ns = now;
            }
            return;
        }
        if (memcmp(spa, tpa, 4) == 0) {
            nt->st.arp_gratuitous++;
        } else if (ip_ok(tpa)) {
            if (from && own) {
                sweep(nt, from, tpa, now, seq);
            }
            if (r->flags & PPC_F_OUT) {
                wait_add(nt, tpa, now, seq);
            }
        }
    } else if (r->a == 2) {
        nt->st.arp_replies++;
        if (!no_sender && memcmp(spa, tpa, 4) == 0) {
            nt->st.arp_gratuitous++;
        }
    } else {
        nt->st.arp_other++;
        return;
    }
    if (no_sender || !ip_ok(spa)) {
        return;
    }
    if (own) {
        host_ip(from, spa);
    } else if (mac_ok(d + 6)) {
        /* Seen with proxies and bridges; also how a forged reply looks. */
        event(nt, PPC_NK_ARP_SENDER, PPC_NS_NOTE, spa, d + 6, NULL, sha, now, seq);
    }
    claim(nt, spa, sha, now, seq);
}

/* ---- DHCP ------------------------------------------------------------------------------- */

struct dh {
    uint8_t op;
    uint8_t type;
    uint8_t bflag;     /* the client asked for a broadcast reply */
    uint8_t has_sid;
    uint8_t has_req;
    uint8_t has_lease;
    uint32_t xid;
    uint32_t lease;
    uint8_t ciaddr[4];
    uint8_t yiaddr[4];
    uint8_t giaddr[4];
    uint8_t chaddr[6];
    uint8_t sid[4];
    uint8_t req[4];
    uint8_t router[4];
    uint8_t dns[4];
    uint8_t mask[4];
    char name[PPC_NET_NAME];
};

/* The parts of a DHCP message this module reads, every one inside the
 * captured bytes. 0, or -1 when it is not a DHCP message with a type. */
static int dh_parse(const uint8_t *d, uint32_t n, uint32_t off, struct dh *m)
{
    const uint8_t *p;
    uint32_t i;

    memset(m, 0, sizeof(*m));
    if (off > n || n - off < DHCP_OPTIONS) {
        return -1;
    }
    p = d + off;
    if (p[1] != 1 || p[2] != 6 || rd32(p + DHCP_FIXED) != DHCP_COOKIE) {
        return -1;
    }
    m->op = p[0];
    m->xid = rd32(p + 4);
    m->bflag = (rd16(p + 10) & 0x8000u) != 0;
    memcpy(m->ciaddr, p + 12, 4);
    memcpy(m->yiaddr, p + 16, 4);
    memcpy(m->giaddr, p + 24, 4);
    memcpy(m->chaddr, p + 28, 6);
    /* At least one byte an option: the loop ends inside n whatever the
     * lengths say. */
    i = off + DHCP_OPTIONS;
    while (i < n) {
        uint8_t code = d[i];
        uint32_t len;
        const uint8_t *v;

        if (code == 0) {
            i++;
            continue;
        }
        if (code == 255 || n - i < 2) {
            break;
        }
        len = d[i + 1];
        if (len > n - i - 2u) {
            break;
        }
        v = d + i + 2;
        switch (code) {
        case 1:
            if (len >= 4) {
                memcpy(m->mask, v, 4);
            }
            break;
        case 3:
            if (len >= 4) {
                memcpy(m->router, v, 4);
            }
            break;
        case 6:
            if (len >= 4) {
                memcpy(m->dns, v, 4);
            }
            break;
        case 12: {
            uint32_t k;

            /* A host name is whatever the client sent: only letters, digits,
             * dot, dash and underscore are kept as they are. */
            for (k = 0; k < len && k < PPC_NET_NAME - 1u; k++) {
                uint8_t c = v[k];

                m->name[k] = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' ||
                                     c == '-' || c == '_'
                                 ? (char)c
                                 : '?';
            }
            m->name[k] = '\0';
            break;
        }
        case 50:
            if (len >= 4) {
                memcpy(m->req, v, 4);
                m->has_req = 1;
            }
            break;
        case 51:
            if (len >= 4) {
                m->lease = rd32(v);
                m->has_lease = 1;
            }
            break;
        case 53:
            if (len >= 1) {
                m->type = v[0];
            }
            break;
        case 54:
            if (len >= 4) {
                memcpy(m->sid, v, 4);
                m->has_sid = 1;
            }
            break;
        default:
            break;
        }
        i += 2u + len;
    }
    return m->type && mac_ok(m->chaddr) ? 0 : -1;
}

static void wait_stop(struct ppc_net *nt, struct ppc_net_dhcp *t)
{
    if (t->wait != PPC_DH_WAIT_NONE) {
        t->wait = PPC_DH_WAIT_NONE;
        if (nt->st.dhcp_waiting) {
            nt->st.dhcp_waiting--;
        }
    }
}

static void wait_start(struct ppc_net *nt, struct ppc_net_dhcp *t, unsigned what, uint64_t now, uint32_t seq)
{
    wait_stop(nt, t);
    t->wait = (uint8_t)what;
    t->wait_ns = now;
    t->seq = seq;
    nt->st.dhcp_waiting++;
}

/* The transaction's entry, or NULL; with `make`, a new one when it is not
 * there (*fresh says so): a free place among the probes, else the one
 * silent longest, whose wait is forgotten unjudged. */
static struct ppc_net_dhcp *tx_of(struct ppc_net *nt, uint32_t xid, const uint8_t *mac, uint64_t now, int make,
                                  int *fresh)
{
    uint8_t x[4] = { (uint8_t)(xid >> 24), (uint8_t)(xid >> 16), (uint8_t)(xid >> 8), (uint8_t)xid };
    uint32_t h = fnv(fnv(2166136261u, x, 4), mac, 6);
    struct ppc_net_dhcp *spare = NULL;
    uint32_t i;

    *fresh = 0;
    for (i = 0; i < NET_PROBES; i++) {
        struct ppc_net_dhcp *t = &nt->dhcp[(h + i) & (PPC_NET_DHCP - 1u)];

        if (t->used && t->xid == xid && memcmp(t->mac, mac, 6) == 0) {
            return t;
        }
        if (!spare || (spare->used && (!t->used || t->last_ns < spare->last_ns))) {
            spare = t;
        }
    }
    if (!make) {
        return NULL;
    }
    if (spare->used && spare->wait != PPC_DH_WAIT_NONE) {
        wait_stop(nt, spare);
        nt->st.dhcp_unjudged++;
    }
    memset(spare, 0, sizeof(*spare));
    memcpy(spare->mac, mac, 6);
    spare->xid = xid;
    spare->used = 1;
    spare->last_ns = now;
    nt->st.dhcp_tx++;
    *fresh = 1;
    return spare;
}

/* The server that sent an OFFER, ACK or NAK: named by its identifier, else
 * by the address it sent from. NULL when the eight places are taken. */
static struct ppc_net_server *server_of(struct ppc_net *nt, const struct ppc_record *r, const struct dh *m,
                                        const uint8_t *d, struct ppc_net_host *from, uint32_t seq)
{
    const uint8_t *sid = m->has_sid ? m->sid : r->src;
    int relayed = memcmp(m->giaddr, none, 4) != 0;
    struct ppc_net_server *s = NULL;
    unsigned i;

    for (i = 0; i < PPC_NET_SERVERS; i++) {
        if (nt->servers[i].used && memcmp(nt->servers[i].ip, sid, 4) == 0) {
            s = &nt->servers[i];
            break;
        }
        if (!nt->servers[i].used && !s) {
            s = &nt->servers[i];
        }
    }
    if (!s) {
        return NULL;
    }
    if (!s->used) {
        memset(s, 0, sizeof(*s));
        memcpy(s->ip, sid, 4);
        s->used = 1;
        s->first_ns = r->rel_ns;
        nt->st.dhcp_servers++;
        if (nt->st.dhcp_servers >= 2) {
            /* Two servers may be a pair set up to share the work, or one
             * nobody meant to have: the capture cannot tell which. */
            event(nt, PPC_NK_DHCP_SERVERS, PPC_NS_WARN, sid, d + 6, nt->servers[0].ip, NULL, r->rel_ns, seq);
        }
    }
    memcpy(s->mac, d + 6, 6);
    s->relayed = (uint8_t)relayed;
    s->seq = seq;
    if (memcmp(m->router, none, 4) != 0) {
        memcpy(s->router, m->router, 4);
    }
    if (memcmp(m->dns, none, 4) != 0) {
        memcpy(s->dns, m->dns, 4);
    }
    if (memcmp(m->mask, none, 4) != 0) {
        memcpy(s->mask, m->mask, 4);
    }
    if (m->has_lease) {
        s->lease_s = m->lease;
    }
    if (from && !relayed && memcmp(r->src, sid, 4) == 0) {
        from->flags |= PPC_NH_DHCP;
        host_ip(from, sid);
    }
    return s;
}

static void lease_add(struct ppc_net *nt, const struct dh *m, const uint8_t *sid, const char *name, uint64_t now,
                      uint32_t seq)
{
    struct ppc_net_lease *l = &nt->leases[nt->lease_next % PPC_NET_LEASES];

    nt->lease_next = (nt->lease_next + 1u) % PPC_NET_LEASES;
    memset(l, 0, sizeof(*l));
    memcpy(l->ip, m->yiaddr, 4);
    memcpy(l->mac, m->chaddr, 6);
    memcpy(l->server, sid, 4);
    l->used = 1;
    l->lease_s = m->has_lease ? m->lease : 0;
    l->seq = seq;
    l->ns = now;
    memcpy(l->name, name, sizeof(l->name));
    l->name[sizeof(l->name) - 1] = '\0';
}

static void dhcp(struct ppc_net *nt, const struct ppc_record *r, const uint8_t *d, uint32_t n, uint32_t pay_off,
                 struct ppc_net_host *from, uint32_t seq)
{
    struct ppc_net_totals *st = &nt->st;
    uint64_t now = r->rel_ns;
    int own = (r->flags & PPC_F_OUT) != 0;
    struct ppc_net_dhcp *t;
    struct ppc_net_server *s;
    struct dh m;
    int fresh;

    if (dh_parse(d, n, pay_off, &m) != 0) {
        return;
    }
    switch (m.type) {
    case 1: /* DISCOVER */
        st->dhcp_discover++;
        t = tx_of(nt, m.xid, m.chaddr, now, 1, &fresh);
        t->last_ns = now;
        if (m.name[0]) {
            memcpy(t->name, m.name, sizeof(t->name));
        }
        if (!(t->seen & PPC_DH_DISCOVER)) {
            /* The first copy: its OFFER is seen here only when it is this
             * device's own, or the client asked for a broadcast reply. */
            if (own || m.bflag) {
                wait_start(nt, t, PPC_DH_WAIT_OFFER, now, seq);
            } else {
                st->dhcp_unseen++;
            }
        }
        t->seen |= PPC_DH_DISCOVER;
        break;
    case 3: /* REQUEST */
        st->dhcp_request++;
        t = tx_of(nt, m.xid, m.chaddr, now, 1, &fresh);
        t->last_ns = now;
        if (m.name[0]) {
            memcpy(t->name, m.name, sizeof(t->name));
        }
        if (m.has_req) {
            memcpy(t->ip, m.req, 4);
        } else if (memcmp(m.ciaddr, none, 4) != 0) {
            memcpy(t->ip, m.ciaddr, 4);
        }
        if (m.has_sid) {
            memcpy(t->server, m.sid, 4);
        }
        if (t->wait == PPC_DH_WAIT_OFFER) {
            wait_stop(nt, t); /* the client chose an offer, seen here or not */
        }
        if (!(t->seen & PPC_DH_REQUEST)) {
            if (own || m.bflag) {
                wait_start(nt, t, PPC_DH_WAIT_ACK, now, seq);
            } else {
                st->dhcp_unseen++;
            }
        }
        t->seen |= PPC_DH_REQUEST;
        break;
    case 4: /* DECLINE: the client found its address in use */
        st->dhcp_decline++;
        t = tx_of(nt, m.xid, m.chaddr, now, 0, &fresh);
        if (t) {
            t->seen |= PPC_DH_DECLINE;
            t->last_ns = now;
        }
        event(nt, PPC_NK_DHCP_DECLINE, PPC_NS_WARN, m.has_req ? m.req : NULL, m.chaddr, m.has_sid ? m.sid : NULL, NULL,
              now, seq);
        break;
    case 7:
        st->dhcp_release++;
        break;
    case 8:
        st->dhcp_inform++;
        break;
    case 2: /* OFFER */
        st->dhcp_offer++;
        s = server_of(nt, r, &m, d, from, seq);
        if (s) {
            s->offers++;
        }
        t = tx_of(nt, m.xid, m.chaddr, now, 1, &fresh);
        t->last_ns = now;
        if (t->wait == PPC_DH_WAIT_OFFER) {
            wait_stop(nt, t);
        }
        if (!(t->seen & PPC_DH_OFFER)) {
            if (t->judged & JUDGED_OFFER) {
                st->dhcp_late++;
            }
            memcpy(t->ip, m.yiaddr, 4);
            memcpy(t->server, m.has_sid ? m.sid : r->src, 4);
        }
        t->seen |= PPC_DH_OFFER;
        break;
    case 5: /* ACK */
        st->dhcp_ack++;
        s = server_of(nt, r, &m, d, from, seq);
        if (s) {
            s->acks++;
        }
        t = tx_of(nt, m.xid, m.chaddr, now, 1, &fresh);
        t->last_ns = now;
        wait_stop(nt, t);
        if (!(t->seen & PPC_DH_ACK)) {
            if (t->judged & JUDGED_ACK) {
                st->dhcp_late++;
            }
            st->dhcp_complete++;
        }
        t->seen |= PPC_DH_ACK;
        if (ip_ok(m.yiaddr)) {
            /* An address was given (an ACK to INFORM gives none). */
            const uint8_t *sid = m.has_sid ? m.sid : r->src;
            struct ppc_net_host *c = host_of(nt, m.chaddr, now, 0);

            memcpy(t->ip, m.yiaddr, 4);
            lease_add(nt, &m, sid, t->name, now, seq);
            if (c) {
                host_ip(c, m.yiaddr);
                if (t->name[0]) {
                    memcpy(c->name, t->name, sizeof(c->name));
                }
            }
            if (ip_ok(m.router)) {
                struct ppc_net_bind *b = bind_of(nt, m.router, now, 1);

                b->router = 1;
                if (b->have) {
                    struct ppc_net_host *g = host_of(nt, b->mac, now, 0);

                    if (g) {
                        g->flags |= PPC_NH_ROUTER;
                    }
                }
            }
            if (m.has_lease && m.lease < PPC_NET_SHORT_LEASE_S) {
                event(nt, PPC_NK_DHCP_SHORT_LEASE, PPC_NS_NOTE, sid, m.chaddr, m.yiaddr, NULL, now, seq);
            }
        }
        break;
    case 6: /* NAK */
        st->dhcp_nak++;
        s = server_of(nt, r, &m, d, from, seq);
        if (s) {
            s->naks++;
        }
        t = tx_of(nt, m.xid, m.chaddr, now, 1, &fresh);
        t->last_ns = now;
        wait_stop(nt, t);
        t->seen |= PPC_DH_NAK;
        event(nt, PPC_NK_DHCP_NAK, PPC_NS_WARN, m.has_sid ? m.sid : r->src, m.chaddr, NULL, NULL, now, seq);
        break;
    default:
        break;
    }
}

/* ---- the frame -------------------------------------------------------------------------- */

void ppc_net_frame(struct ppc_net *nt, const struct ppc_record *r, const uint8_t *d, uint32_t n, uint32_t pay_off,
                   uint32_t seq)
{
    struct ppc_net_host *from;

    if (!nt->mem || !d || n < ETH_HDR) {
        return;
    }
    from = host_of(nt, d + 6, r->rel_ns, 1);
    if (from) {
        if (from->pkts < 0xffffffffu) {
            from->pkts++;
        }
        from->bytes += r->wire_len;
        from->last_ns = r->rel_ns;
        if (r->flags & PPC_F_OUT) {
            from->flags |= PPC_NH_SELF;
        }
    }
    if (r->flags & (PPC_F_MALFORMED | PPC_F_NONFIRST)) {
        return;
    }
    if (r->l3 == PPC_L3_ARP && r->info == PPC_INFO_ARP) {
        arp(nt, r, d, from, seq);
    } else if (r->info == PPC_INFO_DHCP && r->l3 == PPC_L3_IPV4 && r->l4 == 17) {
        dhcp(nt, r, d, n, pay_off, from, seq);
    }
}

int ppc_net_pending(const struct ppc_net *nt)
{
    return nt->mem && (nt->waiting > 0 || nt->st.dhcp_waiting > 0);
}

uint32_t ppc_net_expire(struct ppc_net *nt, uint64_t now_ns)
{
    uint32_t made = 0;
    uint32_t i;

    if (!nt->mem) {
        return 0;
    }
    for (i = 0; i < PPC_NET_DHCP && nt->st.dhcp_waiting > 0; i++) {
        struct ppc_net_dhcp *t = &nt->dhcp[i];

        if (!t->used || t->wait == PPC_DH_WAIT_NONE || since(now_ns, t->wait_ns) < PPC_NET_DHCP_WAIT_NS) {
            continue;
        }
        if (t->wait == PPC_DH_WAIT_OFFER) {
            t->judged |= JUDGED_OFFER;
            nt->st.dhcp_no_offer++;
            event(nt, PPC_NK_DHCP_NO_OFFER, PPC_NS_WARN, NULL, t->mac, NULL, NULL, now_ns, t->seq);
        } else {
            t->judged |= JUDGED_ACK;
            nt->st.dhcp_no_ack++;
            event(nt, PPC_NK_DHCP_NO_ACK, PPC_NS_WARN, ip_ok(t->ip) ? t->ip : NULL, t->mac,
                  ip_ok(t->server) ? t->server : NULL, NULL, now_ns, t->seq);
        }
        wait_stop(nt, t);
        made++;
    }
    for (i = 0; i < PPC_NET_WAITS && nt->waiting > 0; i++) {
        struct ppc_net_wait *w = &nt->waits[i];
        uint64_t age;

        if (!w->used) {
            continue;
        }
        age = since(now_ns, w->last_ns);
        if (w->tries >= PPC_NET_ARP_TRIES && age >= PPC_NET_ARP_WAIT_NS) {
            const struct ppc_net_bind *b = bind_of(nt, w->ip, now_ns, 0);

            /* Nobody answered for an address this device wanted: nothing
             * holds it, or its holder is down. Worse when it is the router. */
            nt->st.arp_noreply++;
            event(nt, PPC_NK_ARP_NO_REPLY, b && b->router ? PPC_NS_WARN : PPC_NS_NOTE, w->ip, NULL, NULL, NULL, now_ns,
                  w->seq);
            made++;
        } else if (age < PPC_NET_ARP_FORGET_NS) {
            continue;
        }
        w->used = 0;
        nt->waiting--;
    }
    return made;
}

void ppc_net_blind(struct ppc_net *nt)
{
    uint32_t i;

    if (!nt->mem) {
        return;
    }
    nt->st.blinded++;
    for (i = 0; i < PPC_NET_DHCP; i++) {
        if (nt->dhcp[i].used && nt->dhcp[i].wait != PPC_DH_WAIT_NONE) {
            nt->dhcp[i].wait = PPC_DH_WAIT_NONE;
            nt->st.dhcp_unjudged++;
        }
    }
    nt->st.dhcp_waiting = 0;
    memset(nt->waits, 0, sizeof(nt->waits));
    nt->waiting = 0;
    for (i = 0; i < PPC_NET_BINDS; i++) {
        nt->binds[i].probed = 0; /* the defence may pass unseen */
    }
    for (i = 0; i < PPC_NET_HOSTS; i++) {
        nt->hosts[i].sweep_bits = 0;
    }
}

/* ---- reading it ------------------------------------------------------------------------- */

void ppc_net_totals(const struct ppc_net *nt, struct ppc_net_totals *out)
{
    unsigned i;

    *out = nt->st;
    out->events_warn = 0;
    out->events_note = 0;
    for (i = 0; i < PPC_NET_EVENTS; i++) {
        if (nt->events[i].used) {
            if (nt->events[i].sev == PPC_NS_WARN) {
                out->events_warn++;
            } else {
                out->events_note++;
            }
        }
    }
}

unsigned ppc_net_top(const struct ppc_net *nt, const struct ppc_net_host **out, unsigned max)
{
    unsigned n = 0;
    uint32_t i;

    for (i = 0; nt->mem && i < PPC_NET_HOSTS && max; i++) {
        const struct ppc_net_host *h = &nt->hosts[i];
        unsigned k;

        if (!h->used || (n == max && h->bytes <= out[n - 1]->bytes)) {
            continue;
        }
        /* Insert it where its bytes put it; the last one falls off a full
         * list. */
        k = n < max ? n++ : max - 1u;
        while (k > 0 && out[k - 1]->bytes < h->bytes) {
            out[k] = out[k - 1];
            k--;
        }
        out[k] = h;
    }
    return n;
}

unsigned ppc_net_servers(const struct ppc_net *nt, const struct ppc_net_server **out, unsigned max)
{
    unsigned n = 0;
    unsigned i;

    for (i = 0; i < PPC_NET_SERVERS && n < max; i++) {
        if (nt->servers[i].used) {
            out[n++] = &nt->servers[i];
        }
    }
    return n;
}

unsigned ppc_net_leases(const struct ppc_net *nt, const struct ppc_net_lease **out, unsigned max)
{
    unsigned n = 0;
    unsigned i;

    for (i = 1; i <= PPC_NET_LEASES && n < max; i++) {
        const struct ppc_net_lease *l = &nt->leases[(nt->lease_next + PPC_NET_LEASES - i) % PPC_NET_LEASES];

        if (l->used) {
            out[n++] = l;
        }
    }
    return n;
}

static int before(const struct ppc_net_event *a, const struct ppc_net_event *b)
{
    return a->sev != b->sev ? a->sev > b->sev : a->last_ns > b->last_ns;
}

unsigned ppc_net_events(const struct ppc_net *nt, const struct ppc_net_event **out, unsigned max)
{
    unsigned n = 0;
    unsigned i;

    for (i = 0; i < PPC_NET_EVENTS && max; i++) {
        const struct ppc_net_event *e = &nt->events[i];
        unsigned k;

        if (!e->used || (n == max && !before(e, out[n - 1]))) {
            continue;
        }
        k = n < max ? n++ : max - 1u;
        while (k > 0 && before(e, out[k - 1])) {
            out[k] = out[k - 1];
            k--;
        }
        out[k] = e;
    }
    return n;
}

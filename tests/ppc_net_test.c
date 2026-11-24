/*
 * Host tests for pocketpcap's network intelligence (core/pocketpcap/
 * ppc_net.h, M12.1): the devices, the ARP pairs and what is and is not
 * called a problem, DHCP transactions and what is and is not called
 * unanswered, the events, and every table's bound.
 *
 * Every frame is built here and goes through the real decoder. Addresses
 * are documentation addresses and locally administered MACs.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketpcap/ppc_decode.h"
#include "pocketpcap/ppc_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(const char *what, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

#define S 1000000000ull

static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t zero[6];

static void mac_of(uint8_t *m, unsigned id)
{
    m[0] = 0x02;
    m[1] = 0x00;
    m[2] = 0x5e;
    m[3] = (uint8_t)(id >> 16);
    m[4] = (uint8_t)(id >> 8);
    m[5] = (uint8_t)id;
}

static void ip_of(uint8_t *ip, unsigned last)
{
    ip[0] = 192;
    ip[1] = 0;
    ip[2] = 2;
    ip[3] = (uint8_t)last;
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v >> 16);
    put16(p + 2, v);
}

static uint32_t next_seq;

/* Decode a frame and hand it over, as the helper does. */
static void feed(struct ppc_net *nt, const uint8_t *b, uint32_t len, uint64_t at_ns, int own)
{
    struct ppc_frame f;
    struct ppc_record r;
    uint32_t pay;

    memset(&f, 0, sizeof(f));
    f.data = b;
    f.cap_len = len;
    f.wire_len = len;
    f.pkttype = own ? PPC_PKT_OUTGOING : PPC_PKT_UNKNOWN;
    pay = ppc_decode(&f, &r);
    r.rel_ns = at_ns;
    ppc_net_frame(nt, &r, b, len, pay, ++next_seq);
}

/* An ARP frame: the Ethernet source `eth` (0: the sender), opcode, sender
 * `s` (MAC id and address, address 0 for none), target address `t`. */
static void arp_x(struct ppc_net *nt, unsigned eth, unsigned op, unsigned smac, unsigned sip, unsigned tip,
                  uint64_t at_ns, int own)
{
    uint8_t b[60];

    memset(b, 0, sizeof(b));
    memcpy(b, op == 1 ? bcast : zero, 6);
    mac_of(b + 6, eth ? eth : smac);
    put16(b + 12, 0x0806);
    put16(b + 14, 1);
    put16(b + 16, 0x0800);
    b[18] = 6;
    b[19] = 4;
    put16(b + 20, op);
    mac_of(b + 22, smac);
    if (sip) {
        ip_of(b + 28, sip);
    }
    ip_of(b + 38, tip);
    feed(nt, b, sizeof(b), at_ns, own);
}

static void arp_req(struct ppc_net *nt, unsigned smac, unsigned sip, unsigned tip, uint64_t at_ns)
{
    arp_x(nt, 0, 1, smac, sip, tip, at_ns, 0);
}

static void arp_reply(struct ppc_net *nt, unsigned smac, unsigned sip, unsigned tip, uint64_t at_ns)
{
    arp_x(nt, 0, 2, smac, sip, tip, at_ns, 0);
}

/* What a DHCP message carries beyond its type. */
struct dhm {
    unsigned type;
    uint32_t xid;
    unsigned client;   /* the client's MAC id */
    unsigned from;     /* the frame's source MAC id (0: the client) */
    unsigned src_ip;   /* the datagram's source, 0 for 0.0.0.0 */
    int bflag;
    unsigned yiaddr;
    unsigned sid;      /* option 54 */
    unsigned req;      /* option 50 */
    uint32_t lease;    /* option 51, 0: none */
    unsigned router;   /* option 3 */
    const char *name;  /* option 12 */
    unsigned giaddr;
};

static uint32_t dhcp_build(uint8_t *b, const struct dhm *m)
{
    int reply = m->type == 2 || m->type == 5 || m->type == 6;
    uint8_t *p;
    uint32_t o;
    uint32_t len;

    memset(b, 0, 600);
    memcpy(b, bcast, 6);
    mac_of(b + 6, m->from ? m->from : m->client);
    put16(b + 12, 0x0800);
    b[14] = 0x45;
    b[22] = 64;
    b[23] = 17;
    if (m->src_ip) {
        ip_of(b + 26, m->src_ip);
    }
    memset(b + 30, 0xff, 4);
    put16(b + 34, reply ? 67 : 68);
    put16(b + 36, reply ? 68 : 67);
    p = b + 42;
    p[0] = reply ? 2 : 1;
    p[1] = 1;
    p[2] = 6;
    put32(p + 4, m->xid);
    put16(p + 10, m->bflag ? 0x8000u : 0);
    if (m->yiaddr) {
        ip_of(p + 16, m->yiaddr);
    }
    if (m->giaddr) {
        ip_of(p + 24, m->giaddr);
    }
    mac_of(p + 28, m->client);
    put32(p + 236, 0x63825363u);
    o = 240;
    p[o++] = 53;
    p[o++] = 1;
    p[o++] = (uint8_t)m->type;
    if (m->sid) {
        p[o++] = 54;
        p[o++] = 4;
        ip_of(p + o, m->sid);
        o += 4;
    }
    if (m->req) {
        p[o++] = 50;
        p[o++] = 4;
        ip_of(p + o, m->req);
        o += 4;
    }
    if (m->lease) {
        p[o++] = 51;
        p[o++] = 4;
        put32(p + o, m->lease);
        o += 4;
    }
    if (m->router) {
        p[o++] = 3;
        p[o++] = 4;
        ip_of(p + o, m->router);
        o += 4;
    }
    if (m->name) {
        size_t nl = strlen(m->name);

        p[o++] = 12;
        p[o++] = (uint8_t)nl;
        memcpy(p + o, m->name, nl);
        o += (uint32_t)nl;
    }
    p[o++] = 255;
    len = 42 + o;
    put16(b + 16, len - 14);
    put16(b + 38, len - 34);
    return len;
}

static void dhcp_send(struct ppc_net *nt, const struct dhm *m, uint64_t at_ns, int own)
{
    uint8_t b[600];

    feed(nt, b, dhcp_build(b, m), at_ns, own);
}

static const struct ppc_net_event *event_of(const struct ppc_net *nt, unsigned kind)
{
    const struct ppc_net_event *ev[PPC_NET_EVENTS];
    unsigned n = ppc_net_events(nt, ev, PPC_NET_EVENTS);
    unsigned i;

    for (i = 0; i < n; i++) {
        if (ev[i]->kind == kind) {
            return ev[i];
        }
    }
    return NULL;
}

static unsigned events(const struct ppc_net *nt)
{
    const struct ppc_net_event *ev[PPC_NET_EVENTS];

    return ppc_net_events(nt, ev, PPC_NET_EVENTS);
}

static const struct ppc_net_host *host_by(const struct ppc_net *nt, unsigned id)
{
    uint8_t m[6];
    unsigned i;

    mac_of(m, id);
    for (i = 0; i < PPC_NET_HOSTS; i++) {
        if (nt->hosts[i].used && memcmp(nt->hosts[i].mac, m, 6) == 0) {
            return &nt->hosts[i];
        }
    }
    return NULL;
}

static int is_mac(const uint8_t *m, unsigned id)
{
    uint8_t want[6];

    mac_of(want, id);
    return memcmp(m, want, 6) == 0;
}

static int is_ip(const uint8_t *ip, unsigned last)
{
    uint8_t want[4];

    ip_of(want, last);
    return memcmp(ip, want, 4) == 0;
}

/* ---- ARP -------------------------------------------------------------------------------- */

static void test_arp_plain(void)
{
    struct ppc_net nt;
    struct ppc_net_totals t;
    const struct ppc_net_host *h;

    check("the tables allocate", ppc_net_init(&nt) == 0);
    arp_req(&nt, 0x10, 10, 1, 0);
    arp_reply(&nt, 0x01, 1, 10, 1000000);
    arp_req(&nt, 0x10, 10, 10, 2 * S); /* an announcement */
    arp_reply(&nt, 0x01, 1, 1, 3 * S); /* a gratuitous reply */
    ppc_net_totals(&nt, &t);
    check("requests, replies and gratuitous ARP are counted apart",
          t.arp_requests == 2 && t.arp_replies == 2 && t.arp_gratuitous == 2 && t.arp_probes == 0);
    check("two addresses have a MAC, two devices were seen", t.arp_binds == 2 && t.hosts == 2);
    h = host_by(&nt, 0x10);
    check("a device's address is the one its own ARP claimed", h && (h->flags & PPC_NH_IP) && is_ip(h->ip, 10));
    check("its frames and bytes are counted", h && h->pkts == 2 && h->bytes == 120);
    check("an ordinary exchange and gratuitous ARP make no event", events(&nt) == 0 && t.events_warn == 0);
    arp_x(&nt, 0, 3, 0x10, 10, 1, 4 * S, 0);
    ppc_net_totals(&nt, &t);
    check("another opcode is counted and claims nothing", t.arp_other == 1 && t.arp_binds == 2);
    ppc_net_free(&nt);
}

static void test_arp_moves(void)
{
    struct ppc_net nt;
    struct ppc_net_totals t;
    const struct ppc_net_event *e;

    ppc_net_init(&nt);
    arp_req(&nt, 0xa, 10, 1, 0);
    arp_req(&nt, 0xb, 10, 1, 100 * S);
    ppc_net_totals(&nt, &t);
    e = event_of(&nt, PPC_NK_ARP_MOVED);
    check("an address that changes MAC once is a note, not a warning",
          t.arp_moved == 1 && e && e->sev == PPC_NS_NOTE && t.events_warn == 0);
    check("the note names the address, the new MAC and the old",
          e && is_ip(e->ip, 10) && is_mac(e->mac, 0xb) && is_mac(e->mac2, 0xa));
    arp_req(&nt, 0xa, 10, 1, 101 * S);
    ppc_net_totals(&nt, &t);
    e = event_of(&nt, PPC_NK_ARP_DUPLICATE);
    check("the first MAC claiming it again a second later: two stations use the address",
          t.arp_flips == 1 && e && e->sev == PPC_NS_WARN && is_mac(e->mac, 0xa) && is_mac(e->mac2, 0xb));
    arp_req(&nt, 0xb, 10, 1, 102 * S);
    arp_req(&nt, 0xa, 10, 1, 103 * S);
    e = event_of(&nt, PPC_NK_ARP_DUPLICATE);
    ppc_net_totals(&nt, &t);
    check("seen again, it is counted on the same event", e && e->count == 3 && t.events_warn == 1);
    check("which names the latest packet and when it began", e && e->seq == next_seq && e->first_ns == 101 * S);

    /* A hand-over and, minutes later, a hand-back: neither is a duplicate. */
    arp_req(&nt, 0xc, 30, 1, 0);
    arp_req(&nt, 0xd, 30, 1, 10 * S);
    arp_req(&nt, 0xc, 30, 1, 200 * S);
    ppc_net_totals(&nt, &t);
    check("a change back after more than a minute is a second move, not a duplicate",
          t.arp_moved == 3 && t.arp_flips == 3);
    ppc_net_free(&nt);
}

static void test_arp_probe(void)
{
    struct ppc_net nt;
    struct ppc_net_totals t;
    const struct ppc_net_event *e;

    ppc_net_init(&nt);
    arp_req(&nt, 0x50, 0, 50, 0);            /* a probe: no sender address */
    arp_reply(&nt, 0x51, 50, 50, 1 * S);     /* its holder defends it */
    ppc_net_totals(&nt, &t);
    e = event_of(&nt, PPC_NK_ARP_CONFLICT);
    check("a probe is counted and claims nothing", t.arp_probes == 1 && t.arp_binds == 1);
    check("a claim from another MAC during the probe is the conflict RFC 5227 probes for",
          t.arp_conflicts == 1 && e && e->sev == PPC_NS_WARN && is_ip(e->ip, 50) && is_mac(e->mac, 0x51) &&
              is_mac(e->mac2, 0x50));
    arp_req(&nt, 0x52, 0, 52, 2 * S);
    arp_req(&nt, 0x52, 52, 52, 4 * S);       /* the prober's own announcement */
    arp_req(&nt, 0x53, 0, 53, 5 * S);
    arp_reply(&nt, 0x54, 53, 1, 30 * S);     /* long after the probe */
    ppc_net_totals(&nt, &t);
    check("the prober's own announcement, and a claim long after, are no conflict", t.arp_conflicts == 1);
    check("nor a move: nobody held those addresses before", t.arp_moved == 0);
    arp_req(&nt, 0x55, 0, 55, 40 * S);
    ppc_net_blind(&nt);
    arp_reply(&nt, 0x56, 55, 55, 41 * S);
    ppc_net_totals(&nt, &t);
    check("a probe is forgotten once frames pass unseen", t.arp_conflicts == 1 && t.blinded == 1);
    ppc_net_free(&nt);
}

static void test_arp_own(void)
{
    struct ppc_net nt;
    struct ppc_net_totals t;
    const struct ppc_net_event *e;
    const struct ppc_net_host *h;

    ppc_net_init(&nt);
    arp_x(&nt, 0, 1, 0x01, 10, 99, 0, 1);
    arp_x(&nt, 0, 1, 0x01, 10, 99, 1 * S, 1);
    check("two requests of this device's own wait", ppc_net_pending(&nt) && ppc_net_expire(&nt, 20 * S) == 0);
    arp_x(&nt, 0, 1, 0x01, 10, 99, 21 * S, 1);
    arp_x(&nt, 0, 1, 0x01, 10, 99, 22 * S, 1);
    arp_x(&nt, 0, 1, 0x01, 10, 99, 23 * S, 1);
    check("three are not judged before two seconds have passed", ppc_net_expire(&nt, 24 * S) == 0);
    check("then they are called unanswered, once", ppc_net_expire(&nt, 25 * S) == 1 && ppc_net_expire(&nt, 26 * S) == 0);
    ppc_net_totals(&nt, &t);
    e = event_of(&nt, PPC_NK_ARP_NO_REPLY);
    check("as a note that names the address and the first request",
          t.arp_noreply == 1 && e && e->sev == PPC_NS_NOTE && is_ip(e->ip, 99) && e->seq == next_seq - 2);
    h = host_by(&nt, 0x01);
    check("the device that sent them is this device", h && (h->flags & PPC_NH_SELF));

    arp_x(&nt, 0, 1, 0x01, 10, 98, 30 * S, 1);
    arp_x(&nt, 0, 1, 0x01, 10, 98, 31 * S, 1);
    arp_x(&nt, 0, 1, 0x01, 10, 98, 32 * S, 1);
    arp_reply(&nt, 0x62, 98, 10, 32 * S + 5000000);
    check("a reply ends the wait", !ppc_net_pending(&nt) && ppc_net_expire(&nt, 40 * S) == 0);

    arp_x(&nt, 0, 1, 0x01, 10, 97, 50 * S, 1);
    arp_x(&nt, 0, 1, 0x01, 10, 97, 51 * S, 1);
    arp_x(&nt, 0, 1, 0x01, 10, 97, 52 * S, 1);
    ppc_net_blind(&nt);
    ppc_net_totals(&nt, &t);
    check("nothing waiting is judged once frames pass unseen",
          !ppc_net_pending(&nt) && ppc_net_expire(&nt, 60 * S) == 0 && t.arp_noreply == 1);

    /* Another station's requests are not judged: its replies are unicast. */
    arp_req(&nt, 0x70, 70, 96, 70 * S);
    arp_req(&nt, 0x70, 70, 96, 71 * S);
    arp_req(&nt, 0x70, 70, 96, 72 * S);
    check("another station's requests are never called unanswered", !ppc_net_pending(&nt));
    ppc_net_free(&nt);
}

static void test_arp_patterns(void)
{
    struct ppc_net nt;
    const struct ppc_net_event *e;
    unsigned i;

    ppc_net_init(&nt);
    for (i = 0; i < 20; i++) {
        arp_req(&nt, 0x80, 80, 100 + i, i * 100000000ull);
    }
    check("twenty requests in two seconds are not a sweep", event_of(&nt, PPC_NK_ARP_SWEEP) == NULL);
    for (i = 20; i < 40; i++) {
        arp_req(&nt, 0x80, 80, 100 + i, i * 100000000ull);
    }
    e = event_of(&nt, PPC_NK_ARP_SWEEP);
    check("forty addresses in four seconds are, as a note with the station's MAC and address",
          e && e->sev == PPC_NS_NOTE && is_mac(e->mac, 0x80) && is_ip(e->ip, 80) && e->count == 1);
    for (i = 0; i < 40; i++) {
        arp_req(&nt, 0x81, 81, 100 + i, 100 * S + i * 2 * S);
    }
    e = event_of(&nt, PPC_NK_ARP_SWEEP);
    check("the same forty over eighty seconds are not", e && e->count == 1 && is_mac(e->mac, 0x80));

    arp_x(&nt, 0x90, 2, 0x91, 91, 1, 300 * S, 0);
    e = event_of(&nt, PPC_NK_ARP_SENDER);
    check("an ARP sender that is not the frame's source is a note naming both",
          e && e->sev == PPC_NS_NOTE && is_ip(e->ip, 91) && is_mac(e->mac, 0x90) && is_mac(e->mac2, 0x91));
    check("and the frame's source is not given the address", host_by(&nt, 0x90) && !(host_by(&nt, 0x90)->flags & PPC_NH_IP));
    {
        struct ppc_net_totals t;

        ppc_net_totals(&nt, &t);
        check("none of these patterns is a warning", t.events_warn == 0 && t.events_note == 2);
    }
    ppc_net_free(&nt);
}

/* ---- DHCP ------------------------------------------------------------------------------- */

static void test_dhcp_whole(void)
{
    struct ppc_net nt;
    struct ppc_net_totals t;
    const struct ppc_net_lease *ls[PPC_NET_LEASES];
    const struct ppc_net_server *sv[PPC_NET_SERVERS];
    const struct ppc_net_host *h;
    const struct ppc_net_event *e;
    struct dhm m;

    ppc_net_init(&nt);
    memset(&m, 0, sizeof(m));
    m.xid = 0x1111;
    m.client = 0x20;
    m.bflag = 1;
    m.name = "desk-pc";
    m.type = 1;
    dhcp_send(&nt, &m, 0, 0);
    dhcp_send(&nt, &m, 4 * S, 0); /* sent again: the same transaction */
    check("a DISCOVER whose OFFER would be broadcast waits", ppc_net_pending(&nt));
    m.name = NULL;
    m.type = 2;
    m.from = 0x01;
    m.src_ip = 1;
    m.sid = 1;
    m.yiaddr = 120;
    m.lease = 86400;
    m.router = 1;
    dhcp_send(&nt, &m, 5 * S, 0);
    check("its OFFER ends the wait", !ppc_net_pending(&nt));
    memset(&m, 0, sizeof(m));
    m.xid = 0x1111;
    m.client = 0x20;
    m.bflag = 1;
    m.type = 3;
    m.sid = 1;
    m.req = 120;
    dhcp_send(&nt, &m, 5 * S + 1000000, 0);
    m.type = 5;
    m.from = 0x01;
    m.src_ip = 1;
    m.req = 0;
    m.yiaddr = 120;
    m.lease = 86400;
    m.router = 1;
    dhcp_send(&nt, &m, 5 * S + 2000000, 0);
    ppc_net_totals(&nt, &t);
    check("DISCOVER, OFFER, REQUEST and ACK are one transaction, acknowledged",
          t.dhcp_tx == 1 && t.dhcp_complete == 1 && t.dhcp_discover == 2 && t.dhcp_offer == 1 && t.dhcp_request == 1 &&
              t.dhcp_ack == 1);
    check("nothing waits, nothing was called unanswered, and there is no event",
          t.dhcp_waiting == 0 && t.dhcp_no_offer == 0 && t.dhcp_no_ack == 0 && events(&nt) == 0 &&
              ppc_net_expire(&nt, 100 * S) == 0);
    check("one server", t.dhcp_servers == 1 && ppc_net_servers(&nt, sv, PPC_NET_SERVERS) == 1 && is_ip(sv[0]->ip, 1) &&
                            is_mac(sv[0]->mac, 0x01) && sv[0]->offers == 1 && sv[0]->acks == 1 && sv[0]->naks == 0);
    check("with the router and lease time it gave", is_ip(sv[0]->router, 1) && sv[0]->lease_s == 86400 && !sv[0]->relayed);
    check("the lease: address, client, server, time, the ACK's packet and the client's name",
          ppc_net_leases(&nt, ls, PPC_NET_LEASES) == 1 && is_ip(ls[0]->ip, 120) && is_mac(ls[0]->mac, 0x20) &&
              is_ip(ls[0]->server, 1) && ls[0]->lease_s == 86400 && ls[0]->seq == next_seq &&
              strcmp(ls[0]->name, "desk-pc") == 0);
    h = host_by(&nt, 0x20);
    check("the client's device has the address and the name", h && (h->flags & PPC_NH_IP) && is_ip(h->ip, 120) &&
                                                                   strcmp(h->name, "desk-pc") == 0);
    h = host_by(&nt, 0x01);
    check("the server's device is marked a DHCP server", h && (h->flags & PPC_NH_DHCP) && is_ip(h->ip, 1));
    check("and not yet the router: no ARP has tied the router's address to it", h && !(h->flags & PPC_NH_ROUTER));
    arp_reply(&nt, 0x01, 1, 120, 6 * S);
    check("once ARP has, it is", h && (h->flags & PPC_NH_ROUTER));
    arp_reply(&nt, 0x66, 1, 120, 500 * S);
    e = event_of(&nt, PPC_NK_ARP_ROUTER_MOVED);
    check("the router's address at another MAC is a warning that names both",
          e && e->sev == PPC_NS_WARN && is_ip(e->ip, 1) && is_mac(e->mac, 0x66) && is_mac(e->mac2, 0x01) &&
              event_of(&nt, PPC_NK_ARP_MOVED) == NULL);
    ppc_net_free(&nt);
}

static struct dhm discover(uint32_t xid, unsigned client, int bflag)
{
    struct dhm m;

    memset(&m, 0, sizeof(m));
    m.type = 1;
    m.xid = xid;
    m.client = client;
    m.bflag = bflag;
    return m;
}

static void test_dhcp_unanswered(void)
{
    struct ppc_net nt;
    struct ppc_net_totals t;
    const struct ppc_net_event *e;
    struct dhm m;

    ppc_net_init(&nt);
    /* Another client, a unicast answer: it cannot be judged from here. */
    m = discover(0x2001, 0x30, 0);
    dhcp_send(&nt, &m, 0, 0);
    ppc_net_totals(&nt, &t);
    check("a DISCOVER whose answer is unicast to another device is not waited for",
          !ppc_net_pending(&nt) && t.dhcp_unseen == 1 && ppc_net_expire(&nt, 60 * S) == 0 && events(&nt) == 0);

    m = discover(0x2002, 0x31, 1);
    dhcp_send(&nt, &m, 100 * S, 0);
    dhcp_send(&nt, &m, 104 * S, 0);
    check("one that asked for a broadcast answer is not judged at 9.9 s", ppc_net_expire(&nt, 109 * S + 900000000) == 0);
    check("and is called unanswered at 10 s, from its first copy, once",
          ppc_net_expire(&nt, 110 * S) == 1 && ppc_net_expire(&nt, 130 * S) == 0);
    ppc_net_totals(&nt, &t);
    e = event_of(&nt, PPC_NK_DHCP_NO_OFFER);
    check("the event names the client and its first DISCOVER",
          t.dhcp_no_offer == 1 && e && e->sev == PPC_NS_WARN && is_mac(e->mac, 0x31) && e->seq == next_seq - 1);
    m.type = 2;
    m.from = 0x01;
    m.src_ip = 1;
    m.sid = 1;
    m.yiaddr = 131;
    dhcp_send(&nt, &m, 131 * S, 0);
    ppc_net_totals(&nt, &t);
    check("an OFFER after that is counted as late", t.dhcp_late == 1 && t.dhcp_no_offer == 1);

    /* This device's own DISCOVER is judged whatever its flag says. */
    m = discover(0x2003, 0x32, 0);
    dhcp_send(&nt, &m, 200 * S, 1);
    check("this device's own DISCOVER is judged without the flag",
          ppc_net_pending(&nt) && ppc_net_expire(&nt, 210 * S) == 1);

    /* A REQUEST nobody acknowledges. */
    memset(&m, 0, sizeof(m));
    m.type = 3;
    m.xid = 0x2004;
    m.client = 0x33;
    m.bflag = 1;
    m.sid = 1;
    m.req = 133;
    dhcp_send(&nt, &m, 300 * S, 0);
    check("a REQUEST with no ACK or NAK in 10 s", ppc_net_expire(&nt, 310 * S) == 1);
    e = event_of(&nt, PPC_NK_DHCP_NO_ACK);
    ppc_net_totals(&nt, &t);
    check("names the client, the address asked for and the server",
          t.dhcp_no_ack == 1 && e && is_mac(e->mac, 0x33) && is_ip(e->ip, 133) && is_ip(e->ip2, 1));

    /* Held: what waits is forgotten, not called unanswered. */
    m = discover(0x2005, 0x34, 1);
    dhcp_send(&nt, &m, 400 * S, 0);
    ppc_net_blind(&nt);
    ppc_net_totals(&nt, &t);
    check("a wait is forgotten unjudged once frames pass unseen",
          !ppc_net_pending(&nt) && t.dhcp_unjudged == 1 && t.dhcp_waiting == 0 && ppc_net_expire(&nt, 500 * S) == 0 &&
              t.dhcp_no_offer == 2);
    ppc_net_free(&nt);
}

static void test_dhcp_trouble(void)
{
    struct ppc_net nt;
    struct ppc_net_totals t;
    const struct ppc_net_event *e;
    struct dhm m;

    ppc_net_init(&nt);
    memset(&m, 0, sizeof(m));
    m.type = 2;
    m.xid = 0x3001;
    m.client = 0x40;
    m.from = 0x01;
    m.src_ip = 1;
    m.sid = 1;
    m.yiaddr = 140;
    dhcp_send(&nt, &m, 0, 0);
    dhcp_send(&nt, &m, 1 * S, 0);
    check("one server answering twice is no event", events(&nt) == 0);
    m.from = 0xfd;
    m.src_ip = 254;
    m.sid = 254;
    m.yiaddr = 240;
    dhcp_send(&nt, &m, 2 * S, 0);
    dhcp_send(&nt, &m, 3 * S, 0);
    ppc_net_totals(&nt, &t);
    e = event_of(&nt, PPC_NK_DHCP_SERVERS);
    check("a second server answering is a warning that names it, its MAC and the first",
          t.dhcp_servers == 2 && e && e->sev == PPC_NS_WARN && is_ip(e->ip, 254) && is_mac(e->mac, 0xfd) &&
              is_ip(e->ip2, 1) && e->count == 1);

    m.type = 6;
    m.from = 0x01;
    m.src_ip = 1;
    m.sid = 1;
    m.yiaddr = 0;
    dhcp_send(&nt, &m, 4 * S, 0);
    e = event_of(&nt, PPC_NK_DHCP_NAK);
    ppc_net_totals(&nt, &t);
    check("a NAK is a warning that names the server and the client",
          t.dhcp_nak == 1 && e && e->sev == PPC_NS_WARN && is_ip(e->ip, 1) && is_mac(e->mac, 0x40));

    memset(&m, 0, sizeof(m));
    m.type = 4;
    m.xid = 0x3002;
    m.client = 0x41;
    m.sid = 1;
    m.req = 141;
    dhcp_send(&nt, &m, 5 * S, 0);
    e = event_of(&nt, PPC_NK_DHCP_DECLINE);
    check("a DECLINE names the address the client found in use",
          e && e->sev == PPC_NS_WARN && is_ip(e->ip, 141) && is_mac(e->mac, 0x41) && is_ip(e->ip2, 1));

    memset(&m, 0, sizeof(m));
    m.type = 5;
    m.xid = 0x3003;
    m.client = 0x42;
    m.from = 0x01;
    m.src_ip = 1;
    m.sid = 1;
    m.yiaddr = 142;
    m.lease = 60;
    dhcp_send(&nt, &m, 6 * S, 0);
    e = event_of(&nt, PPC_NK_DHCP_SHORT_LEASE);
    check("a lease of a minute is a note", e && e->sev == PPC_NS_NOTE && is_ip(e->ip2, 142));
    m.xid = 0x3004;
    m.client = 0x43;
    m.yiaddr = 0; /* an ACK to INFORM: no address given */
    m.lease = 0;
    dhcp_send(&nt, &m, 7 * S, 0);
    {
        const struct ppc_net_lease *ls[PPC_NET_LEASES];

        check("an ACK that gives no address is no lease", ppc_net_leases(&nt, ls, PPC_NET_LEASES) == 1);
    }
    memset(&m, 0, sizeof(m));
    m.type = 2;
    m.xid = 0x3005;
    m.client = 0x44;
    m.from = 0x05;
    m.src_ip = 5;
    m.sid = 9;
    m.giaddr = 5;
    m.yiaddr = 144;
    dhcp_send(&nt, &m, 8 * S, 0);
    check("a relayed answer does not make the relay a DHCP server",
          host_by(&nt, 0x05) && !(host_by(&nt, 0x05)->flags & PPC_NH_DHCP));
    ppc_net_free(&nt);
}

/* ---- hostile and bounded ---------------------------------------------------------------- */

static void test_hostile(void)
{
    struct ppc_net nt;
    struct ppc_net_totals t;
    uint8_t b[600];
    uint8_t *heap;
    struct dhm m;
    uint32_t len;
    uint32_t cut;

    ppc_net_init(&nt);
    memset(&m, 0, sizeof(m));
    m.type = 5;
    m.xid = 0x4001;
    m.client = 0x50;
    m.from = 0x01;
    m.src_ip = 1;
    m.sid = 1;
    m.yiaddr = 150;
    m.lease = 3600;
    m.router = 1;
    m.name = "a-name-much-longer-than-the-table-keeps-of-it";
    len = dhcp_build(b, &m);
    /* Every cut of the frame, each on the heap so a read past it is seen. */
    for (cut = 0; cut <= len; cut++) {
        heap = malloc(cut ? cut : 1);
        memcpy(heap, b, cut);
        feed(&nt, heap, cut, cut * 1000ull, 0);
        free(heap);
    }
    ppc_net_totals(&nt, &t);
    check("every cut of a DHCP ACK is read inside its bytes; only the whole one is an ACK", t.dhcp_ack >= 1);
    {
        /* An option that says it is longer than the frame. */
        uint8_t c[600];

        memcpy(c, b, len);
        c[42 + 240 + 1] = 250;
        feed(&nt, c, len, 1, 0);
        /* A name of bytes no name has. */
        m.xid = 0x4002;
        m.client = 0x51;
        m.type = 1;
        m.from = 0;
        m.src_ip = 0;
        m.sid = 0;
        m.yiaddr = 0;
        m.lease = 0;
        m.router = 0;
        m.name = "ab\tc\x01\x7f<d>";
        m.bflag = 1;
        len = dhcp_build(c, &m);
        feed(&nt, c, len, 2, 0);
        m.type = 5;
        m.from = 0x01;
        m.src_ip = 1;
        m.sid = 1;
        m.yiaddr = 151;
        m.name = NULL;
        len = dhcp_build(c, &m);
        feed(&nt, c, len, 3, 0);
    }
    {
        const struct ppc_net_host *h = host_by(&nt, 0x51);

        check("a host name keeps letters, digits, dot, dash and underscore, and nothing else",
              h && strcmp(h->name, "ab?c??_d_") != 0 && strcmp(h->name, "ab?c???d?") == 0);
    }
    {
        const struct ppc_net_lease *ls[PPC_NET_LEASES];
        unsigned n = ppc_net_leases(&nt, ls, PPC_NET_LEASES);
        unsigned i;
        int cut_ok = 1;

        for (i = 0; i < n; i++) {
            cut_ok &= strlen(ls[i]->name) < PPC_NET_NAME;
        }
        check("a long name is cut to what the table keeps", n >= 1 && cut_ok);
    }
    ppc_net_free(&nt);
}

static void test_bounds(void)
{
    struct ppc_net nt;
    struct ppc_net_totals t;
    const struct ppc_net_host *top[4];
    const struct ppc_net_event *ev[PPC_NET_EVENTS];
    struct dhm m;
    unsigned i;
    unsigned n;

    ppc_net_init(&nt);
    for (i = 1; i <= 5000; i++) {
        arp_req(&nt, 0x10000 + i, 1 + i % 200, 1, i * 1000000ull);
    }
    ppc_net_totals(&nt, &t);
    check("5,000 stations through 256 places: the table never grows",
          t.hosts <= PPC_NET_HOSTS && t.hosts + t.hosts_replaced == 5000 && t.arp_binds <= PPC_NET_BINDS);
    for (i = 0; i < 1000; i++) {
        m = discover(0x5000 + i, 0x20000 + i, 1);
        dhcp_send(&nt, &m, 6 * S + i * 1000000ull, 0);
    }
    ppc_net_totals(&nt, &t);
    check("1,000 DHCP transactions through 64 places: those let go are counted unjudged",
          t.dhcp_tx == 1000 && t.dhcp_waiting <= PPC_NET_DHCP && t.dhcp_waiting + t.dhcp_unjudged == 1000);
    ppc_net_expire(&nt, 100 * S);
    ppc_net_totals(&nt, &t);
    n = ppc_net_events(&nt, ev, PPC_NET_EVENTS);
    check("the events are a ring of 32: the rest are counted lost",
          n == PPC_NET_EVENTS && t.events_warn + t.events_note == PPC_NET_EVENTS && t.events_lost > 0);
    {
        int ordered = 1;

        for (i = 1; i < n; i++) {
            ordered &= ev[i - 1]->sev > ev[i]->sev || (ev[i - 1]->sev == ev[i]->sev && ev[i - 1]->last_ns >= ev[i]->last_ns);
        }
        check("warnings come before notes, the newest first", ordered);
    }
    for (i = 0; i < 9; i++) {
        memset(&m, 0, sizeof(m));
        m.type = 2;
        m.xid = 0x6000;
        m.client = 0x60;
        m.from = 0x100 + i;
        m.src_ip = 200 + i;
        m.sid = 200 + i;
        m.yiaddr = 99;
        dhcp_send(&nt, &m, 200 * S + i, 0);
    }
    ppc_net_totals(&nt, &t);
    check("nine servers through eight places", t.dhcp_servers == PPC_NET_SERVERS);
    ppc_net_free(&nt);

    ppc_net_init(&nt);
    arp_req(&nt, 0xa1, 1, 9, 0);
    for (i = 0; i < 5; i++) {
        arp_req(&nt, 0xa2, 2, 9, 1 + i);
    }
    for (i = 0; i < 3; i++) {
        arp_req(&nt, 0xa3, 3, 9, 10 + i);
    }
    n = ppc_net_top(&nt, top, 2);
    check("the devices that sent the most come first, cut to what was asked for",
          n == 2 && is_mac(top[0]->mac, 0xa2) && is_mac(top[1]->mac, 0xa3));
    {
        uint8_t b[60];

        memset(b, 0, sizeof(b));
        memset(b, 0xff, 6);
        b[6] = 0x01; /* a group address as the source: nothing a station is */
        put16(b + 12, 0x88cc);
        feed(&nt, b, sizeof(b), 20, 0);
        ppc_net_totals(&nt, &t);
        check("a frame from a group address makes no device", t.hosts == 3);
    }
    ppc_net_free(&nt);
    check("freed twice does no harm", (ppc_net_free(&nt), nt.mem == NULL));
}

int main(void)
{
    test_arp_plain();
    test_arp_moves();
    test_arp_probe();
    test_arp_own();
    test_arp_patterns();
    test_dhcp_whole();
    test_dhcp_unanswered();
    test_dhcp_trouble();
    test_hostile();
    test_bounds();
    printf("ppc_net_test: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

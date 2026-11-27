/*
 * The deterministic fixtures of the evidence history (N1.0): five scripted
 * scenes, each a list of timed steps - a frame, a mark, HOLD, frames
 * reported lost, STOP. Shared by tests/ppc_evid_test.c, which plays them
 * through the decoder, the diagnosis, the network intelligence and the
 * evidence history, and tests/ppc_evid_mkfixtures.c, which writes their
 * frames as pcap files for a replay through the real helper.
 *
 * Every frame is built here, byte for byte the same on every run. Addresses
 * are documentation addresses (192.0.2.0/24) and locally administered MACs
 * (02:00:5e:...); names are example.net. No real capture is involved.
 *
 *   dhcp-repeat    another device repeats DISCOVER; its offers are unicast
 *   dns-local      this device asks a name and is answered
 *   unseen-reply   another device joins; the server's replies never pass here
 *   loss-event     two MACs claim one address while frames are being lost
 *   silence        a mark, then nothing relevant for the whole window
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PPC_EVID_FIXTURES_H
#define PPC_EVID_FIXTURES_H

#include "pocketpcap/ppc_record.h"

#include <stdint.h>
#include <string.h>

#define FX_FRAME_MAX 360u
#define FX_STEPS_MAX 64u
#define FX_COUNT 5u

/* MAC ids (02:00:5e:00:00:id) and the last byte of their 192.0.2 address. */
#define FX_SELF 0x10u    /* this device, 192.0.2.10 */
#define FX_ROUTER 0x01u  /* 192.0.2.1, also the DHCP server and the resolver's gateway */
#define FX_DNS 0x35u     /* the resolver, 192.0.2.53 */

enum fx_op { FX_FRAME = 1, FX_MARK, FX_HOLD_ON, FX_HOLD_OFF, FX_LOST, FX_STOP };

struct fx_step {
    uint32_t t_ms;   /* since START */
    uint8_t op;      /* enum fx_op */
    uint8_t dir;     /* FX_FRAME: PPC_PKT_* as a live interface would say it */
    uint16_t len;    /* FX_FRAME: bytes */
    uint32_t n;      /* FX_LOST: frames reported lost at this moment */
    uint8_t frame[FX_FRAME_MAX];
};

struct fx {
    const char *name;
    const char *what;
    uint32_t nsteps;
    struct fx_step step[FX_STEPS_MAX];
};

static inline void fx_mac(uint8_t *m, unsigned id)
{
    m[0] = 0x02;
    m[1] = 0x00;
    m[2] = 0x5e;
    m[3] = 0x00;
    m[4] = (uint8_t)(id >> 8);
    m[5] = (uint8_t)id;
}

static inline void fx_ip(uint8_t *ip, unsigned last)
{
    ip[0] = 192;
    ip[1] = 0;
    ip[2] = 2;
    ip[3] = (uint8_t)last;
}

static inline void fx_put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void fx_put32(uint8_t *p, uint32_t v)
{
    fx_put16(p, v >> 16);
    fx_put16(p + 2, v);
}

static inline struct fx_step *fx_next(struct fx *f, uint32_t t_ms, unsigned op)
{
    struct fx_step *s = &f->step[f->nsteps < FX_STEPS_MAX ? f->nsteps++ : FX_STEPS_MAX - 1u];

    memset(s, 0, sizeof(*s));
    s->t_ms = t_ms;
    s->op = (uint8_t)op;
    return s;
}

/* What a frame's direction is on a switch port: sent by this device, to
 * every station, or to this device. A frame to another station never
 * arrives, so no fixture holds one. */
static inline unsigned fx_dir(unsigned from, int to_all)
{
    return from == FX_SELF ? PPC_PKT_OUTGOING : to_all ? PPC_PKT_BROADCAST : PPC_PKT_HOST;
}

/* ARP: op 1 a request (to every station), op 2 a reply (to `to`). sip 0: a
 * probe, with no sender address. */
static inline void fx_arp(struct fx *f, uint32_t t_ms, unsigned op, unsigned smac, unsigned sip, unsigned tip,
                          unsigned to)
{
    struct fx_step *s = fx_next(f, t_ms, FX_FRAME);
    uint8_t *b = s->frame;

    if (op == 1 || !to) {
        memset(b, 0xff, 6);
    } else {
        fx_mac(b, to);
    }
    fx_mac(b + 6, smac);
    fx_put16(b + 12, 0x0806);
    fx_put16(b + 14, 1);
    fx_put16(b + 16, 0x0800);
    b[18] = 6;
    b[19] = 4;
    fx_put16(b + 20, op);
    fx_mac(b + 22, smac);
    if (sip) {
        fx_ip(b + 28, sip);
    }
    if (op == 2 && to) {
        fx_mac(b + 32, to);
    }
    fx_ip(b + 38, tip);
    s->len = 60;
    s->dir = (uint8_t)fx_dir(smac, op == 1 || !to);
}

/* DHCP. type as on the wire (1 DISCOVER, 2 OFFER, 3 REQUEST, 5 ACK, 6 NAK).
 * A client's message goes to every station; a server's goes to the client
 * alone unless bcast says the client asked for a broadcast reply. */
struct fx_dhcp {
    unsigned type;
    uint32_t xid;
    unsigned client;  /* MAC id */
    unsigned server;  /* MAC id and address of the server (its messages; option 54) */
    unsigned addr;    /* yiaddr of a server's message; option 50 of a client's */
    int bflag;        /* the client's broadcast flag */
    int bcast;        /* a server's message sent to every station */
    uint32_t lease;
};

static inline void fx_dhcp(struct fx *f, uint32_t t_ms, const struct fx_dhcp *m)
{
    struct fx_step *s = fx_next(f, t_ms, FX_FRAME);
    int reply = m->type == 2 || m->type == 5 || m->type == 6;
    uint8_t *b = s->frame;
    uint8_t *p = b + 42;
    uint32_t o = 240;
    uint32_t len;

    if (reply && !m->bcast) {
        fx_mac(b, m->client);
    } else {
        memset(b, 0xff, 6);
    }
    fx_mac(b + 6, reply ? m->server : m->client);
    fx_put16(b + 12, 0x0800);
    b[14] = 0x45;
    b[22] = 64;
    b[23] = 17;
    if (reply) {
        fx_ip(b + 26, m->server);
    }
    if (reply && !m->bcast && m->addr) {
        fx_ip(b + 30, m->addr);
    } else {
        memset(b + 30, 0xff, 4);
    }
    fx_put16(b + 34, reply ? 67 : 68);
    fx_put16(b + 36, reply ? 68 : 67);
    p[0] = reply ? 2 : 1;
    p[1] = 1;
    p[2] = 6;
    fx_put32(p + 4, m->xid);
    fx_put16(p + 10, m->bflag ? 0x8000u : 0);
    if (reply && m->addr) {
        fx_ip(p + 16, m->addr);
    }
    fx_mac(p + 28, m->client);
    fx_put32(p + 236, 0x63825363u);
    p[o++] = 53;
    p[o++] = 1;
    p[o++] = (uint8_t)m->type;
    if (m->server) {
        p[o++] = 54;
        p[o++] = 4;
        fx_ip(p + o, m->server);
        o += 4;
    }
    if (!reply && m->addr) {
        p[o++] = 50;
        p[o++] = 4;
        fx_ip(p + o, m->addr);
        o += 4;
    }
    if (reply && m->lease) {
        p[o++] = 51;
        p[o++] = 4;
        fx_put32(p + o, m->lease);
        o += 4;
    }
    p[o++] = 255;
    len = 42 + o;
    fx_put16(b + 16, len - 14);
    fx_put16(b + 38, len - 34);
    s->len = (uint16_t)len;
    s->dir = (uint8_t)fx_dir(reply ? m->server : m->client, !reply || m->bcast);
}

/* DNS over UDP and IPv4: a query from `client` to the resolver, or the
 * resolver's answer (rcode 0: an address; else a failure without one). */
static inline void fx_dns(struct fx *f, uint32_t t_ms, int answer, unsigned client, uint32_t id, unsigned rcode,
                          const char *name)
{
    struct fx_step *s = fx_next(f, t_ms, FX_FRAME);
    uint8_t *b = s->frame;
    uint32_t o;
    uint32_t q;

    fx_mac(b, answer ? client : FX_DNS);
    fx_mac(b + 6, answer ? FX_DNS : client);
    fx_put16(b + 12, 0x0800);
    b[14] = 0x45;
    b[22] = 64;
    b[23] = 17;
    fx_ip(b + 26, answer ? 53 : client);
    fx_ip(b + 30, answer ? client : 53);
    fx_put16(b + 34, answer ? 53 : 40000u);
    fx_put16(b + 36, answer ? 40000u : 53);
    o = 42;
    fx_put16(b + o, id);
    fx_put16(b + o + 2, answer ? (0x8180u | rcode) : 0x0100u);
    fx_put16(b + o + 4, 1);
    fx_put16(b + o + 6, answer && !rcode ? 1 : 0);
    o += 12;
    q = o;
    while (*name) {
        const char *dot = strchr(name, '.');
        uint32_t l = dot ? (uint32_t)(dot - name) : (uint32_t)strlen(name);

        b[o++] = (uint8_t)l;
        memcpy(b + o, name, l);
        o += l;
        name += l;
        if (*name == '.') {
            name++;
        }
    }
    b[o++] = 0;
    fx_put16(b + o, 1);
    fx_put16(b + o + 2, 1);
    o += 4;
    if (answer && !rcode) {
        fx_put16(b + o, 0xc000u | (q - 42u));
        fx_put16(b + o + 2, 1);
        fx_put16(b + o + 4, 1);
        fx_put32(b + o + 6, 300);
        fx_put16(b + o + 10, 4);
        b[o + 12] = 198;
        b[o + 13] = 51;
        b[o + 14] = 100;
        b[o + 15] = 7;
        o += 16;
    }
    fx_put16(b + 16, o - 14);
    fx_put16(b + 38, o - 34);
    s->len = (uint16_t)(o < 60 ? 60 : o);
    s->dir = (uint8_t)fx_dir(answer ? FX_DNS : client, 0);
}

static inline void fx_op_at(struct fx *f, uint32_t t_ms, unsigned op, uint32_t n)
{
    fx_next(f, t_ms, op)->n = n;
}

/* ---- the five scenes --------------------------------------------------------------------- */

/* Another device (0x21) cannot get an address: it sends DISCOVER five times
 * under one transaction id, without the broadcast flag, so any OFFER goes to
 * it alone and never passes this port. Marked at 50 s, after the fourth. */
static inline void fx_dhcp_repeat(struct fx *f)
{
    static const uint32_t at[5] = { 20000, 24100, 32300, 48500, 64700 };
    struct fx_dhcp m;
    unsigned i;

    memset(f, 0, sizeof(*f));
    f->name = "dhcp-repeat";
    f->what = "another device repeats DHCP DISCOVER";
    fx_arp(f, 2000, 1, FX_ROUTER, 1, 0x10, 0);
    memset(&m, 0, sizeof(m));
    m.type = 1;
    m.xid = 0x5e210001u;
    m.client = 0x21;
    for (i = 0; i < 4; i++) {
        fx_dhcp(f, at[i], &m);
    }
    fx_op_at(f, 50000, FX_MARK, 0);
    fx_dhcp(f, at[4], &m);
    fx_arp(f, 90000, 1, FX_ROUTER, 1, 0x10, 0);
    fx_op_at(f, 120000, FX_STOP, 0);
}

/* This device resolves a name: it asks the router's MAC for the resolver's
 * address, is told, asks the name and is answered 31 ms later. Marked at
 * 75 s; the capture stops 5 s after the mark. */
static inline void fx_dns_local(struct fx *f)
{
    memset(f, 0, sizeof(*f));
    f->name = "dns-local";
    f->what = "this device asks a name and is answered";
    fx_arp(f, 69900, 1, FX_SELF, 0x10, 53, 0);
    fx_arp(f, 69901, 2, FX_DNS, 53, 0x10, FX_SELF);
    fx_dns(f, 70000, 0, FX_SELF, 0x1234, 0, "www.example.net");
    fx_dns(f, 70031, 1, FX_SELF, 0x1234, 0, "www.example.net");
    fx_op_at(f, 75000, FX_MARK, 0);
    fx_op_at(f, 80000, FX_STOP, 0);
}

/* Another device (0x31) joins: DISCOVER and REQUEST go to every station,
 * the server's OFFER and ACK go to it alone and are never seen here. That
 * it was answered shows only in what it does next: it probes the address,
 * then announces it. Marked at 30 s. */
static inline void fx_unseen_reply(struct fx *f)
{
    struct fx_dhcp m;

    memset(f, 0, sizeof(*f));
    f->name = "unseen-reply";
    f->what = "another device is answered by unicast replies this port never sees";
    memset(&m, 0, sizeof(m));
    m.xid = 0x5e310001u;
    m.client = 0x31;
    m.type = 1;
    fx_dhcp(f, 10000, &m);
    m.type = 3;
    m.server = FX_ROUTER;
    m.addr = 57;
    fx_dhcp(f, 10200, &m);
    fx_arp(f, 10500, 1, 0x31, 0, 57, 0);
    fx_arp(f, 12500, 1, 0x31, 57, 57, 0);
    fx_op_at(f, 30000, FX_MARK, 0);
    fx_op_at(f, 100000, FX_STOP, 0);
}

/* Two stations (0x41, 0x42) claim 192.0.2.77 in turn, and a server refuses
 * a third (0x44) with a broadcast NAK, while the interface reports 500
 * frames lost over three seconds. What was seen stays seen. Marked at 45 s. */
static inline void fx_loss_event(struct fx *f)
{
    struct fx_dhcp m;

    memset(f, 0, sizeof(*f));
    f->name = "loss-event";
    f->what = "an address conflict and a NAK observed while frames are lost";
    fx_arp(f, 40500, 1, 0x41, 77, 77, 0);
    fx_op_at(f, 40900, FX_LOST, 120);
    fx_arp(f, 41200, 1, 0x42, 77, 77, 0);
    fx_op_at(f, 41800, FX_LOST, 300);
    fx_arp(f, 41900, 1, 0x41, 77, 77, 0);
    fx_op_at(f, 42700, FX_LOST, 80);
    memset(&m, 0, sizeof(m));
    m.type = 6;
    m.xid = 0x5e440001u;
    m.client = 0x44;
    m.server = FX_ROUTER;
    m.bcast = 1;
    fx_dhcp(f, 43000, &m);
    fx_op_at(f, 45000, FX_MARK, 0);
    fx_op_at(f, 110000, FX_STOP, 0);
}

/* Ordinary background, a mark at 30 s, then nothing that concerns DHCP, ARP
 * or DNS for the whole minute after it: the window closes by the clock. */
static inline void fx_silence(struct fx *f)
{
    memset(f, 0, sizeof(*f));
    f->name = "silence";
    f->what = "a mark followed by a silent minute";
    fx_arp(f, 5000, 1, FX_ROUTER, 1, 0x10, 0);
    fx_arp(f, 15000, 1, FX_ROUTER, 1, 0x10, 0);
    fx_dns(f, 20000, 0, FX_SELF, 0x2001, 0, "time.example.net");
    fx_dns(f, 20018, 1, FX_SELF, 0x2001, 0, "time.example.net");
    fx_op_at(f, 30000, FX_MARK, 0);
    fx_op_at(f, 100000, FX_STOP, 0);
}

static inline void fx_build(unsigned i, struct fx *f)
{
    switch (i) {
    case 0:
        fx_dhcp_repeat(f);
        break;
    case 1:
        fx_dns_local(f);
        break;
    case 2:
        fx_unseen_reply(f);
        break;
    case 3:
        fx_loss_event(f);
        break;
    default:
        fx_silence(f);
        break;
    }
}

#endif

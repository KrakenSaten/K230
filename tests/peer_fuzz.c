/*
 * peerd under a fuzzer: the frame parser and the whole receive path of the
 * engine (services/peerd/peer_wire.c, peer_core.c), fed bytes from the
 * network's point of view.
 *
 * The target is LLVMFuzzerTestOneInput, so libFuzzer can drive it:
 *
 *   clang -fsanitize=fuzzer,address,undefined ... tests/peer_fuzz.c ...
 *
 * The build host has gcc only, so with -DPEER_FUZZ_STANDALONE=1 this file
 * also brings a driver of its own (main, below): random bytes at every
 * interesting length, and mutations of genuine frames - a beacon, a probe, a
 * lookup and sealed datagrams from a known node and from a kept one. It is
 * built with the address and undefined-behaviour sanitizers
 * (make peerd-fuzz).
 *
 * Besides "does not crash or read out of bounds", every input is held to an
 * invariant: a frame that is not byte for byte a genuine one is never
 * accepted - not as a datagram, not as a signed frame - and never changes
 * who the victim's nodes are.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "peer_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORT 47230
#define VICTIM_ADDR 0x0A00000Au
#define FRIEND_ADDR 0x0A00000Bu
#define KEPT_ADDR 0x0A00000Cu
#define BCAST 0x0A0000FFu
#define TEMPLATES 8

struct end {
    struct pc_identity id;
    struct pcore *core;
    uint32_t addr;
};

static struct end victim;
static struct end friend_;      /* a node the victim has heard */
static struct end kept;         /* a peer the victim keeps */
static uint8_t tmpl[TEMPLATES][PW_FRAME_MAX];
static size_t tmpl_len[TEMPLATES];
static uint32_t tmpl_src[TEMPLATES];
static int n_tmpl;
static int capture_on;
static int64_t now = 1000000;
static int ready;

static int op_send(void *user, uint32_t dst, uint16_t port, const uint8_t *buf, size_t n, int bcast)
{
    struct end *e = user;

    (void)dst;
    (void)port;
    (void)bcast;
    if (capture_on && e != &victim && n_tmpl < TEMPLATES && n <= PW_FRAME_MAX) {
        memcpy(tmpl[n_tmpl], buf, n);
        tmpl_len[n_tmpl] = n;
        tmpl_src[n_tmpl] = e->addr;
        n_tmpl++;
    }
    return 0;
}

static uint32_t op_source_for(void *user, uint32_t dst)
{
    (void)dst;
    return ((struct end *)user)->addr;
}

static int op_kept_save(void *user, const struct ps_kept *recs, int n)
{
    (void)user;
    (void)recs;
    (void)n;
    return 0;
}

static const struct pcore_ops OPS = { op_send, op_source_for, op_kept_save, NULL };

static void make(struct end *e, uint32_t addr, uint64_t epoch)
{
    struct pcore_iface ifc = { addr, BCAST };

    pc_identity_generate(&e->id);
    e->addr = addr;
    e->core = pcore_new(&e->id, epoch, "fuzz", PORT, &OPS, e, NULL, 0);
    pcore_set_ifaces(e->core, &ifc, 1);
}

/* Hand one end's next frames to the victim (and keep copies as templates). */
static void say(struct end *e, uint8_t type)
{
    int before = n_tmpl;
    int i;

    capture_on = 1;
    switch (type) {
    case PW_BEACON:
        pcore_set_visible(e->core, 1, now);
        pcore_tick(e->core, now);
        break;
    case PW_PROBE:
        pcore_probe(e->core, VICTIM_ADDR, PORT, now);
        pcore_tick(e->core, now);
        break;
    default:
        pcore_send_app(e->core, victim.id.pk, 1, (const uint8_t *)"a genuine datagram", 18, now);
        break;
    }
    capture_on = 0;
    for (i = before; i < n_tmpl; i++) {
        pcore_input(victim.core, tmpl_src[i], PORT, tmpl[i], tmpl_len[i], now);
    }
}

static void setup(void)
{
    if (pc_init() != 0) {
        abort();
    }
    make(&victim, VICTIM_ADDR, 5);
    make(&friend_, FRIEND_ADDR, 9);
    make(&kept, KEPT_ADDR, 3);
    pcore_set_visible(victim.core, 1, now);
    /* The victim hears both, keeps one; both learn the victim. */
    say(&friend_, PW_BEACON);
    say(&kept, PW_BEACON);
    pcore_keep(victim.core, kept.id.pk);
    {
        /* So the two can address the victim: its beacon, by hand. */
        capture_on = 0;
        pcore_keep(friend_.core, victim.id.pk);
        pcore_keep(kept.core, victim.id.pk);
        pcore_tick(victim.core, now);
    }
    /* Give both the victim's address the way a lookup reply would: a kept
     * peer with no address is looked for; here it is simply told. */
    {
        struct ps_kept rec;

        memset(&rec, 0, sizeof(rec));
        memcpy(rec.key, victim.id.pk, PC_KEY);
        rec.addr = VICTIM_ADDR;
        rec.port = PORT;
        pcore_free(friend_.core);
        friend_.core = pcore_new(&friend_.id, 10, "fuzz", PORT, &OPS, &friend_, &rec, 1);
        pcore_free(kept.core);
        kept.core = pcore_new(&kept.id, 4, "fuzz", PORT, &OPS, &kept, &rec, 1);
        {
            struct pcore_iface a = { FRIEND_ADDR, BCAST };
            struct pcore_iface b = { KEPT_ADDR, BCAST };

            pcore_set_ifaces(friend_.core, &a, 1);
            pcore_set_ifaces(kept.core, &b, 1);
        }
    }
    n_tmpl = 0;
    say(&friend_, PW_BEACON);
    say(&kept, PW_BEACON);
    say(&friend_, PW_PROBE);
    say(&friend_, PW_DATA);
    say(&kept, PW_DATA);
    /* Two the victim has NOT seen: mutations of these are near-misses of
     * frames that would be accepted. */
    capture_on = 1;
    pcore_send_app(friend_.core, victim.id.pk, 1, (const uint8_t *)"not yet delivered", 17, now);
    pcore_send_app(kept.core, victim.id.pk, 2, (const uint8_t *)"nor this", 8, now);
    capture_on = 0;
    if (n_tmpl < 6) {
        fprintf(stderr, "fuzz: setup made only %d templates\n", n_tmpl);
        abort();
    }
    ready = 1;
}

static uint64_t accepted(void)
{
    return pcore_counter(victim.core, PCORE_C_ACCEPTED_DATA) +
           pcore_counter(victim.core, PCORE_C_ACCEPTED_SIGNED);
}

static int node_census(void)
{
    static struct pcore_node_info info[PCORE_NODES];

    return pcore_nodes(victim.core, info, PCORE_NODES, now);
}

/* 1 when data is exactly one of the genuine frames. */
static int genuine(const uint8_t *data, size_t size)
{
    int i;

    for (i = 0; i < n_tmpl; i++) {
        if (size == tmpl_len[i] && memcmp(data, tmpl[i], size) == 0) {
            return 1;
        }
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static uint8_t frame[PW_FRAME_MAX + 64];
    uint64_t before;
    int nodes;
    uint32_t src;

    if (!ready) {
        setup();
    }
    if (size < 2) {
        pcore_input(victim.core, FRIEND_ADDR, PORT, data, size, now);
        return 0;
    }
    /* Byte 0: which genuine frame to start from, or none. Byte 1: where it
     * comes from. The rest: the bytes, or the changes to make. */
    src = data[1] & 1 ? FRIEND_ADDR : (data[1] & 2 ? KEPT_ADDR : 0x0A000000u | data[1]);
    if ((data[0] & 0x0F) < n_tmpl) {
        int t = data[0] & 0x0F;
        size_t len = tmpl_len[t];
        size_t i;

        memcpy(frame, tmpl[t], len);
        for (i = 2; i + 2 < size; i += 3) {
            size_t at = (((size_t)data[i] << 8) | data[i + 1]) % len;

            frame[at] ^= data[i + 2];
        }
        if (data[0] & 0x10 && len > 1) {
            len -= 1 + (data[1] >> 2) % (len - 1);      /* cut short */
        } else if (data[0] & 0x20) {
            len += (data[1] >> 2) % 60;                 /* grown, with whatever follows */
        }
        before = accepted();
        nodes = node_census();
        pcore_input(victim.core, src, PORT, frame, len, now);
        if (!genuine(frame, len) && (accepted() != before || node_census() != nodes)) {
            fprintf(stderr, "fuzz: a frame that is not genuine was accepted (template %d, %zu bytes)\n",
                    t, len);
            abort();
        }
    } else {
        size_t len = size - 2 > sizeof(frame) ? sizeof(frame) : size - 2;

        memcpy(frame, data + 2, len);
        before = accepted();
        nodes = node_census();
        pcore_input(victim.core, src, PORT, frame, len, now);
        if (!genuine(frame, len) && (accepted() != before || node_census() != nodes)) {
            fprintf(stderr, "fuzz: %zu arbitrary bytes were accepted\n", len);
            abort();
        }
    }
    /* Time passes too, so ticks, budgets and beacons are exercised. */
    now += data[1] & 0x80 ? 300 : 1;
    pcore_set_visible(victim.core, 1, now);
    pcore_tick(victim.core, now);
    return 0;
}

#ifdef PEER_FUZZ_STANDALONE
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint64_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

int main(int argc, char **argv)
{
    static uint8_t in[PW_FRAME_MAX + 200];
    static const size_t lengths[] = { 0, 1, 2, 3, 57, 58, 59, 60, 99, 100, 101, 102, 209, 210,
                                      211, 212, 213, 1124, 1125, 1126, 1127, 1300 };
    long runs = argc > 1 ? strtol(argv[1], NULL, 10) : 200000;
    long i;
    size_t k;

    if (argc > 2) {
        rng_state = strtoull(argv[2], NULL, 0) | 1;
    }
    /* Every interesting length once, with every type byte. */
    for (k = 0; k < sizeof(lengths) / sizeof(lengths[0]); k++) {
        int type;

        for (type = 0; type < 256; type++) {
            size_t j;

            for (j = 0; j < lengths[k]; j++) {
                in[j] = (uint8_t)rng();
            }
            if (lengths[k] > 3) {
                in[0] = 0xFF;                   /* no template: raw bytes */
                in[2] = PW_VERSION;
                in[3] = (uint8_t)type;
            }
            LLVMFuzzerTestOneInput(in, lengths[k]);
        }
    }
    for (i = 0; i < runs; i++) {
        size_t len;
        size_t j;
        uint64_t r = rng();

        switch (r % 4) {
        case 0:         /* a genuine frame with a few changes */
            len = 2 + 3 * (1 + (r >> 8) % 4);
            break;
        case 1:         /* one changed bit, more or less */
            len = 5;
            break;
        case 2:         /* raw bytes with a plausible header */
            len = 2 + (r >> 8) % (PW_FRAME_MAX + 100);
            break;
        default:        /* anything */
            len = (r >> 8) % sizeof(in);
            break;
        }
        for (j = 0; j < len; j++) {
            in[j] = (uint8_t)rng();
        }
        if (r % 4 == 2 && len > 4) {
            in[0] = 0xFF;
            in[2] = PW_VERSION;
            in[3] = (uint8_t)(1 + (r >> 40) % 6);
        }
        if (r % 4 == 1) {
            in[4] = (uint8_t)(1u << ((r >> 44) % 8));   /* a single bit */
        }
        if (r % 4 <= 1) {
            in[0] &= 0x3F;
            in[0] = (uint8_t)((in[0] & 0xF0) | ((r >> 32) % (unsigned)n_tmpl));
        }
        LLVMFuzzerTestOneInput(in, len);
    }
    printf("peer_fuzz: %ld inputs and %zu fixed ones, no crash, nothing forged accepted\n", runs,
           sizeof(lengths) / sizeof(lengths[0]) * 256);
    printf("     the victim saw %llu frames: %llu malformed, %llu failed authentication, %llu replays,\n"
           "     %llu unknown senders; %llu datagrams and %llu signed frames accepted (the genuine ones)\n",
           (unsigned long long)pcore_counter(victim.core, PCORE_C_RX),
           (unsigned long long)pcore_counter(victim.core, PCORE_C_MALFORMED),
           (unsigned long long)pcore_counter(victim.core, PCORE_C_AUTH),
           (unsigned long long)pcore_counter(victim.core, PCORE_C_REPLAY),
           (unsigned long long)pcore_counter(victim.core, PCORE_C_UNKNOWN),
           (unsigned long long)pcore_counter(victim.core, PCORE_C_ACCEPTED_DATA),
           (unsigned long long)pcore_counter(victim.core, PCORE_C_ACCEPTED_SIGNED));
    pcore_free(victim.core);
    pcore_free(friend_.core);
    pcore_free(kept.core);
    pc_identity_wipe(&victim.id);
    pc_identity_wipe(&friend_.id);
    pc_identity_wipe(&kept.id);
    return 0;
}
#endif

/*
 * pocketpcap view: the display filter and sort. See ppc_view.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ppc_view.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

/* ---- the filter ------------------------------------------------------------------------ */

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

static int parse_mac(const char *t, uint8_t out[6])
{
    int i;

    if (strlen(t) != 17) {
        return -1;
    }
    for (i = 0; i < 6; i++) {
        int hi = hexval(t[i * 3]);
        int lo = hexval(t[i * 3 + 1]);

        if (hi < 0 || lo < 0 || (i < 5 && t[i * 3 + 2] != ':')) {
            return -1;
        }
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}

int ppc_filter_host(const char *text, uint8_t out[16])
{
    struct in_addr a4;
    struct in6_addr a6;

    if (!text || !*text || strlen(text) > 45) {
        return -1;
    }
    memset(out, 0, 16);
    if (parse_mac(text, out) == 0) {
        return 6;
    }
    if (inet_pton(AF_INET, text, &a4) == 1) {
        memcpy(out, &a4, 4);
        return 4;
    }
    if (inet_pton(AF_INET6, text, &a6) == 1) {
        memcpy(out, &a6, 16);
        return 16;
    }
    return -1;
}

/* Whether the record's ends, as ppc_record.h lays them out, include host. */
static int host_match(const struct ppc_filter *f, const struct ppc_record *r)
{
    switch (f->host_len) {
    case 4:
        if (r->l3 == PPC_L3_IPV4 || r->l3 == PPC_L3_ARP) {
            return memcmp(r->src, f->host, 4) == 0 || memcmp(r->dst, f->host, 4) == 0;
        }
        return 0;
    case 16:
        return r->l3 == PPC_L3_IPV6 && (memcmp(r->src, f->host, 16) == 0 || memcmp(r->dst, f->host, 16) == 0);
    case 6:
        if (r->l3 == PPC_L3_ARP) {
            return memcmp(r->src + 4, f->host, 6) == 0 || memcmp(r->dst + 4, f->host, 6) == 0;
        }
        if (r->l3 == PPC_L3_NONE || r->l3 == PPC_L3_OTHER) {
            return memcmp(r->src, f->host, 6) == 0 || memcmp(r->dst, f->host, 6) == 0;
        }
        return 0;
    default:
        return 1;
    }
}

int ppc_filter_match(const struct ppc_filter *f, const struct ppc_record *r)
{
    if (f->classes && f->classes != PPC_CLASSES_ALL && (r->cls >= PPC_CLASS_COUNT || !(f->classes & (1u << r->cls)))) {
        return 0;
    }
    if (f->diag && ppc_sig_diag(r->signal) != f->diag) {
        return 0;
    }
    if (f->signals && !(r->signal & f->signals)) {
        return 0;
    }
    if (f->port) {
        if ((r->l4 != 6 && r->l4 != 17) || (r->flags & PPC_F_NONFIRST) || (r->sport != f->port && r->dport != f->port)) {
            return 0;
        }
    }
    return host_match(f, r);
}

static const char *const sort_names[PPC_SORT_COUNT] = { "time", "newest", "len" };

int ppc_sort_parse(const char *word)
{
    int i;

    for (i = 0; word && i < PPC_SORT_COUNT; i++) {
        if (strcmp(word, sort_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

const char *ppc_sort_name(enum ppc_sort s)
{
    return (unsigned)s < PPC_SORT_COUNT ? sort_names[s] : "time";
}

/* ---- the view ------------------------------------------------------------------------- */

int ppc_view_init(struct ppc_view *v, uint32_t cap)
{
    memset(v, 0, sizeof(*v));
    if (cap == 0 || (cap & (cap - 1u)) != 0) {
        return -1;
    }
    v->idx = malloc((size_t)cap * sizeof(uint32_t) * 3u);
    if (!v->idx) {
        return -1;
    }
    v->sorted = v->idx + cap;
    v->tmp = v->sorted + cap;
    v->cap = cap;
    v->f.classes = PPC_CLASSES_ALL;
    return 0;
}

void ppc_view_free(struct ppc_view *v)
{
    free(v->idx);
    memset(v, 0, sizeof(*v));
}

static uint32_t ring_at(const struct ppc_view *v, uint32_t pos)
{
    return v->idx[(v->head + pos) & (v->cap - 1u)];
}

void ppc_view_set(struct ppc_view *v, const struct ppc_store *s, uint32_t id, enum ppc_sort sort,
                  const struct ppc_filter *f)
{
    v->id = id;
    v->sort = (unsigned)sort < PPC_SORT_COUNT ? sort : PPC_SORT_TIME;
    v->f = *f;
    v->head = 0;
    v->count = 0;
    v->scanned = s && s->first_seq > 0 ? s->first_seq - 1u : 0;
    v->nsorted = 0;
    v->sorted_valid = 0;
    v->dirty = 1;
    v->npins = 0;
    ppc_view_update(v, s);
}

void ppc_view_update(struct ppc_view *v, const struct ppc_store *s)
{
    uint32_t seq;

    if (!v->idx || !s || !s->sum) {
        return;
    }
    /* What the store dropped leaves the front. */
    while (v->count > 0 && ring_at(v, 0) < s->first_seq) {
        v->head = (v->head + 1u) & (v->cap - 1u);
        v->count--;
        v->dirty = 1;
    }
    if (v->scanned + 1u < s->first_seq) {
        v->scanned = s->first_seq - 1u;
    }
    for (seq = v->scanned + 1u; seq != 0 && seq <= s->last_seq; seq++) {
        const struct ppc_record *r = ppc_store_get(s, seq);

        v->scanned = seq;
        if (r && ppc_filter_match(&v->f, r) && v->count < v->cap) {
            v->idx[(v->head + v->count) & (v->cap - 1u)] = seq;
            v->count++;
            v->dirty = 1;
        }
    }
}

static uint32_t sort_key(const struct ppc_store *s, uint32_t seq)
{
    const struct ppc_record *r = ppc_store_get(s, seq);
    uint32_t len = r ? r->wire_len : 0;

    /* Largest first: the key counts down. */
    return 0xffffu - (len > 0xffffu ? 0xffffu : len);
}

/* A stable two-pass radix sort of the ring by key, low byte then high. */
static void build_len_order(struct ppc_view *v, const struct ppc_store *s)
{
    uint32_t count[256];
    uint32_t i;
    int pass;

    for (pass = 0; pass < 2; pass++) {
        uint32_t sum = 0;
        int shift = pass * 8;

        memset(count, 0, sizeof(count));
        for (i = 0; i < v->count; i++) {
            uint32_t seq = pass == 0 ? ring_at(v, i) : v->tmp[i];

            count[(sort_key(s, seq) >> shift) & 0xffu]++;
        }
        for (i = 0; i < 256; i++) {
            uint32_t c = count[i];

            count[i] = sum;
            sum += c;
        }
        for (i = 0; i < v->count; i++) {
            uint32_t seq = pass == 0 ? ring_at(v, i) : v->tmp[i];
            uint32_t *out = pass == 0 ? v->tmp : v->sorted;

            out[count[(sort_key(s, seq) >> shift) & 0xffu]++] = seq;
        }
    }
    v->nsorted = v->count;
}

static const struct ppc_record *pin_rec(const struct ppc_store *s, uint32_t seq)
{
    const struct ppc_pin *p = ppc_store_pinned(s, seq);

    return p ? &p->rec : NULL;
}

/* The pins that pass the filter, in the view's order (a handful: insertion). */
static void build_pins(struct ppc_view *v, const struct ppc_store *s)
{
    uint32_t i;
    uint32_t j;

    v->npins = 0;
    for (i = 0; i < PPC_PINS; i++) {
        const struct ppc_pin *p = &s->pins[i];

        if (p->seq && ppc_filter_match(&v->f, &p->rec)) {
            v->pins[v->npins++] = p->seq;
        }
    }
    for (i = 1; i < v->npins; i++) {
        uint32_t x = v->pins[i];

        for (j = i; j > 0; j--) {
            const struct ppc_record *a = pin_rec(s, v->pins[j - 1]);
            const struct ppc_record *b = pin_rec(s, x);
            int after;

            switch (v->sort) {
            case PPC_SORT_NEWEST:
                after = v->pins[j - 1] < x;
                break;
            case PPC_SORT_LEN:
                after = a->wire_len < b->wire_len || (a->wire_len == b->wire_len && v->pins[j - 1] > x);
                break;
            case PPC_SORT_TIME:
            default:
                after = v->pins[j - 1] > x;
                break;
            }
            if (!after) {
                break;
            }
            v->pins[j] = v->pins[j - 1];
        }
        v->pins[j] = x;
    }
}

void ppc_view_prepare(struct ppc_view *v, const struct ppc_store *s, uint64_t now_ms)
{
    if (!v->idx || !s) {
        return;
    }
    if (v->f.pinned) {
        build_pins(v, s);
        return;
    }
    if (v->sort != PPC_SORT_LEN) {
        return;
    }
    if (!v->sorted_valid || (v->dirty && now_ms >= v->sorted_ms + PPC_VIEW_RESORT_MS)) {
        build_len_order(v, s);
        v->sorted_valid = 1;
        v->dirty = 0;
        v->sorted_ms = now_ms;
    }
}

uint32_t ppc_view_count(const struct ppc_view *v)
{
    if (v->f.pinned) {
        return v->npins;
    }
    return v->sort == PPC_SORT_LEN ? v->nsorted : v->count;
}

uint32_t ppc_view_at(const struct ppc_view *v, uint32_t rank)
{
    if (rank >= ppc_view_count(v)) {
        return 0;
    }
    if (v->f.pinned) {
        return v->pins[rank];
    }
    switch (v->sort) {
    case PPC_SORT_NEWEST:
        return ring_at(v, v->count - 1u - rank);
    case PPC_SORT_LEN:
        return v->sorted[rank];
    case PPC_SORT_TIME:
    default:
        return ring_at(v, rank);
    }
}

/* The first ring position whose seq is >= seq (count when none). */
static uint32_t lower_bound(const struct ppc_view *v, uint32_t seq)
{
    uint32_t lo = 0;
    uint32_t hi = v->count;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;

        if (ring_at(v, mid) < seq) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo;
}

uint32_t ppc_view_rank_of(const struct ppc_view *v, uint32_t seq, int *exact)
{
    uint32_t n = ppc_view_count(v);
    uint32_t i;
    uint32_t pos;

    if (exact) {
        *exact = 0;
    }
    if (n == 0) {
        return 0;
    }
    if (v->f.pinned || v->sort == PPC_SORT_LEN) {
        for (i = 0; i < n; i++) {
            if (ppc_view_at(v, i) == seq) {
                if (exact) {
                    *exact = 1;
                }
                return i;
            }
        }
        return 0;
    }
    pos = lower_bound(v, seq);
    if (pos < v->count && ring_at(v, pos) == seq && exact) {
        *exact = 1;
    }
    if (v->sort == PPC_SORT_NEWEST) {
        /* The nearest earlier packet, or the newest when seq is past them all. */
        if (pos < v->count && ring_at(v, pos) == seq) {
            return v->count - 1u - pos;
        }
        return pos == 0 ? v->count - 1u : v->count - pos;
    }
    return pos < v->count ? pos : v->count - 1u;
}

uint32_t ppc_view_rank_of_time(const struct ppc_view *v, const struct ppc_store *s, uint64_t rel_ns)
{
    uint32_t lo = 0;
    uint32_t hi;

    if (v->f.pinned || v->sort == PPC_SORT_LEN || v->count == 0) {
        return 0;
    }
    hi = v->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        const struct ppc_record *r = ppc_store_get(s, ring_at(v, mid));

        if (r && r->rel_ns < rel_ns) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (v->sort == PPC_SORT_NEWEST) {
        /* The newest packet of that second: the last one before the next. */
        const struct ppc_record *r = lo < v->count ? ppc_store_get(s, ring_at(v, lo)) : NULL;
        uint32_t pos = lo;

        if (!r || r->rel_ns >= rel_ns + 1000000000ull) {
            pos = lo == 0 ? 0 : lo - 1u;
        } else {
            while (pos + 1u < v->count) {
                const struct ppc_record *n = ppc_store_get(s, ring_at(v, pos + 1u));

                if (!n || n->rel_ns >= rel_ns + 1000000000ull) {
                    break;
                }
                pos++;
            }
        }
        return v->count - 1u - pos;
    }
    return lo < v->count ? lo : v->count - 1u;
}

const struct ppc_record *ppc_view_record(const struct ppc_store *s, uint32_t seq, const struct ppc_pin **pin)
{
    const struct ppc_record *r = ppc_store_get(s, seq);
    const struct ppc_pin *p = NULL;

    if (!r) {
        p = ppc_store_pinned(s, seq);
        r = p ? &p->rec : NULL;
    }
    if (pin) {
        *pin = p;
    }
    return r;
}

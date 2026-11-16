/*
 * pocketpcap store. See ppc_store.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ppc_store.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int pow2(uint64_t v)
{
    return v && (v & (v - 1)) == 0;
}

static uint64_t round8(uint64_t v)
{
    return (v + 7u) & ~(uint64_t)7u;
}

int ppc_store_init(struct ppc_store *s, uint32_t summaries, uint64_t ring_bytes)
{
    memset(s, 0, sizeof(*s));
    if (!pow2(summaries) || !pow2(ring_bytes) || ring_bytes < 64) {
        errno = EINVAL;
        return -1;
    }
    /* Plain malloc, not calloc: pages are touched as the rings fill, so a
     * short capture does not cost the whole budget in resident memory. The
     * budget is still the bound: nothing is ever allocated after this. */
    s->sum = malloc((size_t)summaries * sizeof(*s->sum));
    s->ring = malloc((size_t)ring_bytes);
    if (!s->sum || !s->ring) {
        free(s->sum);
        free(s->ring);
        memset(s, 0, sizeof(*s));
        errno = ENOMEM;
        return -1;
    }
    s->sum_cap = summaries;
    s->ring_size = ring_bytes;
    s->first_seq = 1;
    return 0;
}

void ppc_store_free(struct ppc_store *s)
{
    free(s->sum);
    free(s->ring);
    memset(s, 0, sizeof(*s));
}

uint32_t ppc_store_count(const struct ppc_store *s)
{
    return s->sum && s->last_seq >= s->first_seq ? s->last_seq - s->first_seq + 1u : 0u;
}

uint32_t ppc_store_fill_permille(const struct ppc_store *s)
{
    uint64_t a;
    uint64_t b;

    if (!s->sum_cap || !s->ring_size) {
        return 0;
    }
    a = (uint64_t)ppc_store_count(s) * 1000u / s->sum_cap;
    b = (s->head - s->tail) * 1000u / s->ring_size;
    return (uint32_t)(a > b ? a : b);
}

/* ---- the byte ring ------------------------------------------------------------------- */

static void ring_put(struct ppc_store *s, uint64_t at, const void *src, uint64_t n)
{
    uint64_t i = at & (s->ring_size - 1u);
    uint64_t first = s->ring_size - i;

    if (first >= n) {
        memcpy(s->ring + i, src, (size_t)n);
    } else {
        memcpy(s->ring + i, src, (size_t)first);
        memcpy(s->ring, (const uint8_t *)src + first, (size_t)(n - first));
    }
}

static void ring_get(const struct ppc_store *s, uint64_t at, void *dst, uint64_t n)
{
    uint64_t i = at & (s->ring_size - 1u);
    uint64_t first = s->ring_size - i;

    if (first >= n) {
        memcpy(dst, s->ring + i, (size_t)n);
    } else {
        memcpy(dst, s->ring + i, (size_t)first);
        memcpy((uint8_t *)dst + first, s->ring, (size_t)(n - first));
    }
}

/* Drop the oldest entries until `need` more bytes fit. */
static void make_room(struct ppc_store *s, uint64_t need)
{
    while (s->head - s->tail + need > s->ring_size && s->tail < s->head) {
        uint32_t hdr[2];

        ring_get(s, s->tail, hdr, sizeof(hdr));
        s->tail += PPC_STORE_ENTRY_HDR + round8(hdr[1]);
        if (s->tail > s->head) {
            s->tail = s->head; /* cannot happen with entries we wrote; never trust it */
        }
    }
}

uint32_t ppc_store_add(struct ppc_store *s, struct ppc_record *r, const uint8_t *bytes, uint32_t n)
{
    uint32_t seq;
    uint64_t need;

    if (!s->sum || s->last_seq == UINT32_MAX) {
        return 0;
    }
    seq = s->last_seq + 1u;
    if (ppc_store_count(s) == s->sum_cap) {
        s->first_seq++;
        s->evicted++;
    }
    s->last_seq = seq;
    r->seq = seq;
    if (n > 65535u) {
        n = 65535u;
    }
    need = PPC_STORE_ENTRY_HDR + round8(n);
    if (!bytes || n == 0 || need > s->ring_size) {
        if (n > 0) {
            s->bytes_dropped++;
        }
        r->cap_len = 0;
        r->frame_off = s->head;
    } else {
        uint32_t hdr[2] = { seq, n };

        make_room(s, need);
        r->frame_off = s->head;
        r->cap_len = (uint16_t)n;
        ring_put(s, s->head, hdr, sizeof(hdr));
        ring_put(s, s->head + PPC_STORE_ENTRY_HDR, bytes, n);
        s->head += need;
    }
    s->sum[(seq - 1u) & (s->sum_cap - 1u)] = *r;
    return seq;
}

const struct ppc_record *ppc_store_get(const struct ppc_store *s, uint32_t seq)
{
    const struct ppc_record *r;

    if (!s->sum || seq == 0 || seq < s->first_seq || seq > s->last_seq) {
        return NULL;
    }
    r = &s->sum[(seq - 1u) & (s->sum_cap - 1u)];
    return r->seq == seq ? r : NULL;
}

int ppc_store_has_bytes(const struct ppc_store *s, const struct ppc_record *r)
{
    uint32_t hdr[2];

    if (!r || r->cap_len == 0 || r->frame_off < s->tail ||
        r->frame_off + PPC_STORE_ENTRY_HDR + r->cap_len > s->head) {
        return 0;
    }
    ring_get(s, r->frame_off, hdr, sizeof(hdr));
    return hdr[0] == r->seq && hdr[1] == r->cap_len;
}

uint32_t ppc_store_copy(const struct ppc_store *s, const struct ppc_record *r, uint8_t *out,
                        uint32_t max)
{
    uint32_t n;

    if (!out || !ppc_store_has_bytes(s, r)) {
        return 0;
    }
    n = r->cap_len < max ? r->cap_len : max;
    ring_get(s, r->frame_off + PPC_STORE_ENTRY_HDR, out, n);
    return n;
}

/* ---- pins ----------------------------------------------------------------------------- */

const struct ppc_pin *ppc_store_pinned(const struct ppc_store *s, uint32_t seq)
{
    int i;

    for (i = 0; seq && i < PPC_PINS; i++) {
        if (s->pins[i].seq == seq) {
            return &s->pins[i];
        }
    }
    return NULL;
}

int ppc_store_pin(struct ppc_store *s, uint32_t seq)
{
    const struct ppc_record *r;
    int i;

    if (ppc_store_pinned(s, seq)) {
        return 0;
    }
    r = ppc_store_get(s, seq);
    if (!r) {
        return -1;
    }
    for (i = 0; i < PPC_PINS; i++) {
        if (s->pins[i].seq == 0) {
            s->pins[i].rec = *r;
            s->pins[i].n = ppc_store_copy(s, r, s->pins[i].bytes, PPC_PIN_BYTES);
            s->pins[i].seq = seq;
            return 0;
        }
    }
    return -2;
}

int ppc_store_unpin(struct ppc_store *s, uint32_t seq)
{
    int i;

    for (i = 0; seq && i < PPC_PINS; i++) {
        if (s->pins[i].seq == seq) {
            s->pins[i].seq = 0;
            s->pins[i].n = 0;
            return 0;
        }
    }
    return -1;
}

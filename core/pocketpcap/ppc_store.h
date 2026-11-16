/*
 * pocketpcap store: where the capture helper keeps a session's packets
 * (DESIGN_DECISIONS D1, D8, D9; owner-accepted D1 and D10).
 *
 * Two rings, both allocated once when the session starts and never grown:
 *
 *   - a summary ring of `summaries` 80-byte records, indexed by seq: slot
 *     (seq - 1) & (summaries - 1). The valid range is first_seq..last_seq;
 *     when it is full the oldest record is dropped;
 *   - a byte ring of `ring_bytes` holding each record's stored bytes behind
 *     an 8-byte header, addressed by a MONOTONIC 64-bit offset. A record's
 *     frame_off never changes and is never reused; its bytes are present
 *     while frame_off >= tail. The oldest bytes are dropped to make room.
 *
 * So a stale reference cannot name a different packet: a seq below
 * first_seq, or an offset below tail, is simply gone (D8). Summaries and
 * bytes expire independently; a record can outlive its bytes (shown as
 * "summary only"), never the reverse in a way anyone can observe.
 *
 * Pins (D9): up to PPC_PINS packets copied (record and at most
 * PPC_PIN_BYTES of bytes) so they survive the rings turning.
 *
 * Nothing here allocates after ppc_store_init(), blocks, or knows about
 * time or sockets.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PPC_STORE_H
#define PPC_STORE_H

#include "ppc_record.h"

#include <stddef.h>
#include <stdint.h>

#define PPC_STORE_SUMMARIES (64u * 1024u)
#define PPC_STORE_RING_BYTES (16u * 1024u * 1024u)
#define PPC_PINS 8
#define PPC_PIN_BYTES 4096u
#define PPC_STORE_ENTRY_HDR 8u

struct ppc_pin {
    uint32_t seq;   /* 0: free */
    uint32_t n;     /* bytes kept, at most PPC_PIN_BYTES */
    struct ppc_record rec;
    uint8_t bytes[PPC_PIN_BYTES];
};

struct ppc_store {
    struct ppc_record *sum;
    uint32_t sum_cap;   /* a power of two */
    uint8_t *ring;
    uint64_t ring_size; /* a power of two, a multiple of 8 */
    uint64_t head;      /* next byte to write: monotonic */
    uint64_t tail;      /* oldest byte still valid: monotonic */
    uint32_t first_seq; /* oldest summary held; last_seq + 1 when empty */
    uint32_t last_seq;  /* newest summary; 0 before the first */
    uint64_t evicted;   /* summaries dropped to make room */
    uint64_t bytes_dropped; /* frames whose bytes did not fit at all */
    struct ppc_pin pins[PPC_PINS];
};

/* Allocate. summaries and ring_bytes must be powers of two (ring_bytes at
 * least 64). Returns 0, or -1 (errno ENOMEM or EINVAL) with nothing held. */
int ppc_store_init(struct ppc_store *s, uint32_t summaries, uint64_t ring_bytes);
void ppc_store_free(struct ppc_store *s);

/* Store one packet: r gets the next seq, its frame_off and its cap_len (n,
 * or 0 when even an empty ring could not hold n bytes). Returns the seq, or
 * 0 when the sequence space is exhausted (nothing stored). */
uint32_t ppc_store_add(struct ppc_store *s, struct ppc_record *r, const uint8_t *bytes, uint32_t n);

/* The record with this seq, or NULL when it was never stored or has been
 * dropped. Valid until the next ppc_store_add(). */
const struct ppc_record *ppc_store_get(const struct ppc_store *s, uint32_t seq);

/* Whether r's bytes are still in the byte ring. */
int ppc_store_has_bytes(const struct ppc_store *s, const struct ppc_record *r);

/* Copy up to max of r's stored bytes into out. Returns the count, 0 when
 * they have left the ring (or there were none). */
uint32_t ppc_store_copy(const struct ppc_store *s, const struct ppc_record *r, uint8_t *out,
                        uint32_t max);

/* Summaries held. */
uint32_t ppc_store_count(const struct ppc_store *s);
/* How full, in thousandths: the fuller of the two rings. */
uint32_t ppc_store_fill_permille(const struct ppc_store *s);

/* Pin seq: 0, -1 when it is not held, -2 when every pin slot is taken.
 * Pinning a pinned seq is 0 and changes nothing. */
int ppc_store_pin(struct ppc_store *s, uint32_t seq);
/* 0, or -1 when seq is not pinned. */
int ppc_store_unpin(struct ppc_store *s, uint32_t seq);
const struct ppc_pin *ppc_store_pinned(const struct ppc_store *s, uint32_t seq);

/* Add findings (PPC_SIG_*) to a packet already stored, and to its pin: a
 * finding decided after the packet came (M7, the unanswered SYN). 0, or -1
 * when neither the store nor a pin holds it. */
int ppc_store_mark(struct ppc_store *s, uint32_t seq, uint16_t signal);

#endif

/*
 * PacketShark's bounded store (core/pocketpcap/ppc_store.h).
 *
 * What is checked: sequence numbers start at 1 and are never reused; the
 * summary ring drops its oldest record when full and a dropped seq is gone
 * (never another packet's); the byte ring drops bytes before summaries when
 * frames are large, and summaries before bytes when the summary ring is the
 * smaller; offsets are monotonic and an evicted record's bytes are never
 * returned; a record bigger than the whole ring keeps its summary without
 * bytes; the fill figure; pins: a pinned packet stays whole after 200,000
 * further packets (PRODUCT_SPEC F7), eight slots, unpin frees; and memory
 * exhaustion: an allocation that cannot be made is refused cleanly.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketpcap/ppc_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    } else {
        printf("ok   %s\n", what);
    }
}

/* Bytes of packet `seq`: recognisable, so a copy can be checked. */
static void fill(uint8_t *b, uint32_t n, uint32_t seq)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        b[i] = (uint8_t)(seq * 31u + i);
    }
}

static int bytes_ok(const uint8_t *b, uint32_t n, uint32_t seq)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (b[i] != (uint8_t)(seq * 31u + i)) {
            return 0;
        }
    }
    return 1;
}

static uint32_t add(struct ppc_store *s, uint32_t n)
{
    static uint8_t b[65536];
    struct ppc_record r;
    uint32_t seq = s->last_seq + 1u;

    memset(&r, 0, sizeof(r));
    r.wire_len = n;
    fill(b, n, seq);
    return ppc_store_add(s, &r, b, n);
}

static void test_summaries_wrap(void)
{
    struct ppc_store s;
    uint32_t i;
    int ok = 1;

    check("init 16 summaries, 4 KiB of bytes", ppc_store_init(&s, 16, 4096) == 0);
    check("empty: nothing held", ppc_store_count(&s) == 0 && ppc_store_get(&s, 1) == NULL &&
                                     ppc_store_get(&s, 0) == NULL);
    for (i = 1; i <= 40; i++) {
        ok = ok && add(&s, 20) == i;
    }
    check("40 packets get seq 1..40", ok);
    check("16 held, 24 dropped", ppc_store_count(&s) == 16 && s.first_seq == 25 && s.last_seq == 40 &&
                                     s.evicted == 24);
    check("a dropped seq is gone, not someone else's", ppc_store_get(&s, 24) == NULL &&
                                                           ppc_store_get(&s, 8) == NULL);
    check("held seqs are theirs", ppc_store_get(&s, 25) && ppc_store_get(&s, 25)->seq == 25 &&
                                      ppc_store_get(&s, 40) && ppc_store_get(&s, 40)->seq == 40);
    check("past the newest is nothing", ppc_store_get(&s, 41) == NULL);
    {
        uint8_t b[64];
        const struct ppc_record *r = ppc_store_get(&s, 33);

        check("summaries expire first here: the bytes of 33 are whole",
              r && ppc_store_copy(&s, r, b, sizeof(b)) == 20 && bytes_ok(b, 20, 33));
    }
    ppc_store_free(&s);
}

static void test_bytes_first(void)
{
    struct ppc_store s;
    uint8_t b[2048];
    const struct ppc_record *r;
    uint64_t last_off = 0;
    uint32_t i;
    int mono = 1;

    ppc_store_init(&s, 1024, 8192);
    for (i = 1; i <= 50; i++) {
        add(&s, 1000);
        r = ppc_store_get(&s, i);
        if (!r || (i > 1 && r->frame_off <= last_off)) {
            mono = 0;
        }
        last_off = r ? r->frame_off : last_off;
    }
    check("offsets only grow", mono);
    check("all 50 summaries held, the bytes ring holds the last few", ppc_store_count(&s) == 50);
    r = ppc_store_get(&s, 1);
    check("packet 1: summary yes, bytes no", r && !ppc_store_has_bytes(&s, r) && ppc_store_copy(&s, r, b, sizeof(b)) == 0);
    r = ppc_store_get(&s, 50);
    check("packet 50: whole", r && ppc_store_copy(&s, r, b, sizeof(b)) == 1000 && bytes_ok(b, 1000, 50));
    r = ppc_store_get(&s, 45);
    check("packet 45: whole across the ring's end", r && ppc_store_copy(&s, r, b, sizeof(b)) == 1000 &&
                                                        bytes_ok(b, 1000, 45));
    check("ring fill is near full", ppc_store_fill_permille(&s) >= 800);
    i = add(&s, 9000);
    r = ppc_store_get(&s, i);
    check("bigger than the whole ring: summary, no bytes", r && r->cap_len == 0 && !ppc_store_has_bytes(&s, r) &&
                                                               s.bytes_dropped == 1);
    ppc_store_free(&s);
}

static void test_pins(void)
{
    struct ppc_store s;
    const struct ppc_pin *p;
    uint32_t i;
    int k;

    ppc_store_init(&s, 1024, 64 * 1024);
    for (i = 0; i < 10; i++) {
        add(&s, 300);
    }
    check("pin 5", ppc_store_pin(&s, 5) == 0);
    check("pin 5 again changes nothing", ppc_store_pin(&s, 5) == 0);
    for (i = 0; i < 200000; i++) {
        add(&s, 100 + i % 1400);
    }
    p = ppc_store_pinned(&s, 5);
    check("after 200,000 more packets 5 is gone from the rings (F7)", ppc_store_get(&s, 5) == NULL);
    check("but its pin is whole: record and bytes (F7)",
          p && p->rec.seq == 5 && p->n == 300 && bytes_ok(p->bytes, 300, 5));
    check("a seq no longer held cannot be pinned", ppc_store_pin(&s, 6) == -1);
    for (k = 0; k < PPC_PINS - 1; k++) {
        ppc_store_pin(&s, s.last_seq - (uint32_t)k);
    }
    check("eight slots, then full", ppc_store_pin(&s, s.last_seq - 20) == -2);
    check("unpin frees a slot", ppc_store_unpin(&s, 5) == 0 && ppc_store_pinned(&s, 5) == NULL &&
                                    ppc_store_pin(&s, s.last_seq - 20) == 0);
    check("unpin of what is not pinned", ppc_store_unpin(&s, 5) == -1 && ppc_store_unpin(&s, 0) == -1);
    ppc_store_free(&s);
}

static void test_limits(void)
{
    struct ppc_store s;

    check("summaries must be a power of two", ppc_store_init(&s, 1000, 4096) == -1 && errno == EINVAL);
    check("the ring must be a power of two", ppc_store_init(&s, 1024, 5000) == -1 && errno == EINVAL);
    check("memory exhaustion: refused, nothing held",
          ppc_store_init(&s, 1u << 30, 1ull << 42) == -1 && errno == ENOMEM && s.sum == NULL && s.ring == NULL);
    check("an unusable store stores nothing", ppc_store_count(&s) == 0 && ppc_store_get(&s, 1) == NULL);
    ppc_store_init(&s, 16, 4096);
    s.last_seq = UINT32_MAX - 1u;
    s.first_seq = UINT32_MAX - 1u;
    check("the last seq", add(&s, 10) == UINT32_MAX);
    check("then nothing: no seq is ever reused", add(&s, 10) == 0 && s.last_seq == UINT32_MAX);
    ppc_store_free(&s);
    check("the production sizes allocate", ppc_store_init(&s, PPC_STORE_SUMMARIES, PPC_STORE_RING_BYTES) == 0);
    ppc_store_free(&s);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_summaries_wrap();
    test_bytes_first();
    test_pins();
    test_limits();
    printf("ppc_store_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}

/*
 * PacketShark's packet-list view (core/pocketpcap/ppc_view.h), milestone M5.
 *
 * What is checked: the host words a view accepts (IPv4, IPv6, MAC) and
 * refuses; the filter - a class, a port at either end, a host at either
 * end, a MAC only where the record keeps MACs (ARP, frames with no network
 * layer); the time order and its reverse; the index kept up as packets are
 * stored and trimmed as the store drops them (never a seq the store no
 * longer holds, never one twice); ranks of a seq, exact and nearest, and of
 * a time; the length order (largest first, ties in capture order) and its
 * rebuild at most once a second; the pinned view, whose packets outlive the
 * store's; and a seq that has left everything is gone.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketpcap/ppc_store.h"
#include "pocketpcap/ppc_view.h"

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

static const uint8_t A4[4] = { 192, 0, 2, 10 };
static const uint8_t B4[4] = { 192, 0, 2, 20 };
static const uint8_t A6[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0a };
static const uint8_t MAC[6] = { 0x02, 0x00, 0x5e, 0x10, 0x00, 0x02 };

/* Packet i of a pattern: TCP to port 443 (i % 4 == 0), UDP DNS (1), ARP from
 * MAC (2), IPv6 (3); length grows with i % 7; 10 ms apart. */
static uint32_t add(struct ppc_store *s, uint32_t i)
{
    struct ppc_record r;
    uint8_t bytes[64];

    memset(&r, 0, sizeof(r));
    memset(bytes, (int)i, sizeof(bytes));
    r.rel_ns = (uint64_t)i * 10000000u;
    r.wire_len = 60u + (i % 7u) * 100u;
    switch (i % 4u) {
    case 0:
        r.cls = PPC_CLASS_TCP;
        r.l3 = PPC_L3_IPV4;
        r.l4 = 6;
        r.sport = 50000;
        r.dport = 443;
        memcpy(r.src, A4, 4);
        memcpy(r.dst, B4, 4);
        break;
    case 1:
        r.cls = PPC_CLASS_DNS;
        r.l3 = PPC_L3_IPV4;
        r.l4 = 17;
        r.sport = 53;
        r.dport = 40000;
        memcpy(r.src, B4, 4);
        memcpy(r.dst, A4, 4);
        break;
    case 2:
        r.cls = PPC_CLASS_ARP;
        r.l3 = PPC_L3_ARP;
        memcpy(r.src, B4, 4);
        memcpy(r.src + 4, MAC, 6);
        break;
    default:
        r.cls = PPC_CLASS_IPV6;
        r.l3 = PPC_L3_IPV6;
        r.l4 = 58;
        memcpy(r.src, A6, 16);
        break;
    }
    return ppc_store_add(s, &r, bytes, sizeof(bytes));
}

static int ascending(const struct ppc_view *v)
{
    uint32_t i;

    for (i = 1; i < ppc_view_count(v); i++) {
        if (ppc_view_at(v, i) <= ppc_view_at(v, i - 1)) {
            return 0;
        }
    }
    return 1;
}

static void test_host_words(void)
{
    uint8_t h[16];

    check("IPv4, IPv6 and MAC host words", ppc_filter_host("192.0.2.10", h) == 4 && memcmp(h, A4, 4) == 0 &&
                                               ppc_filter_host("2001:db8::a", h) == 16 &&
                                               memcmp(h, A6, 16) == 0 &&
                                               ppc_filter_host("02:00:5e:10:00:02", h) == 6 &&
                                               memcmp(h, MAC, 6) == 0);
    check("anything else is refused", ppc_filter_host("", h) < 0 && ppc_filter_host("192.0.2", h) < 0 &&
                                          ppc_filter_host("example.net", h) < 0 &&
                                          ppc_filter_host("02:00:5e:10:00", h) < 0 && ppc_filter_host(NULL, h) < 0);
    check("sort words", ppc_sort_parse("time") == PPC_SORT_TIME && ppc_sort_parse("newest") == PPC_SORT_NEWEST &&
                            ppc_sort_parse("len") == PPC_SORT_LEN && ppc_sort_parse("size") < 0);
}

static void test_filter(void)
{
    struct ppc_store s;
    struct ppc_view v;
    struct ppc_filter f;
    uint32_t i;
    int ok = 1;

    ppc_store_init(&s, 1024, 1u << 16);
    ppc_view_init(&v, 1024);
    for (i = 0; i < 100; i++) {
        add(&s, i);
    }
    memset(&f, 0, sizeof(f));
    f.classes = PPC_CLASSES_ALL;
    ppc_view_set(&v, &s, 1, PPC_SORT_TIME, &f);
    check("every packet in capture order", ppc_view_count(&v) == 100 && ppc_view_at(&v, 0) == 1 &&
                                               ppc_view_at(&v, 99) == 100 && ascending(&v) && ppc_view_at(&v, 100) == 0);
    f.classes = 1u << PPC_CLASS_TCP;
    ppc_view_set(&v, &s, 2, PPC_SORT_TIME, &f);
    for (i = 0; i < ppc_view_count(&v); i++) {
        ok = ok && ppc_store_get(&s, ppc_view_at(&v, i))->cls == PPC_CLASS_TCP;
    }
    check("one class: its 25 packets, in order", ppc_view_count(&v) == 25 && ok && ascending(&v));
    f.classes = PPC_CLASSES_ALL;
    f.port = 443;
    ppc_view_set(&v, &s, 3, PPC_SORT_TIME, &f);
    check("a port at either end: TCP to 443", ppc_view_count(&v) == 25);
    f.port = 53;
    ppc_view_set(&v, &s, 3, PPC_SORT_TIME, &f);
    check("a port as the source: DNS from 53", ppc_view_count(&v) == 25 && ppc_store_get(&s, ppc_view_at(&v, 0))->cls ==
                                                                              PPC_CLASS_DNS);
    f.port = 0;
    f.host_len = (uint8_t)ppc_filter_host("192.0.2.10", f.host);
    ppc_view_set(&v, &s, 4, PPC_SORT_TIME, &f);
    check("an IPv4 host at either end: TCP and DNS", ppc_view_count(&v) == 50);
    f.host_len = (uint8_t)ppc_filter_host("192.0.2.20", f.host);
    ppc_view_set(&v, &s, 4, PPC_SORT_TIME, &f);
    check("and ARP's sender address", ppc_view_count(&v) == 75);
    f.host_len = (uint8_t)ppc_filter_host("2001:db8::a", f.host);
    ppc_view_set(&v, &s, 5, PPC_SORT_TIME, &f);
    check("an IPv6 host", ppc_view_count(&v) == 25);
    f.host_len = (uint8_t)ppc_filter_host("02:00:5e:10:00:02", f.host);
    ppc_view_set(&v, &s, 6, PPC_SORT_TIME, &f);
    check("a MAC: where the record keeps one (ARP), not an IP packet's", ppc_view_count(&v) == 25 &&
                                                                         ppc_store_get(&s, ppc_view_at(&v, 3))->cls ==
                                                                             PPC_CLASS_ARP);
    f.host_len = 0;
    f.classes = (1u << PPC_CLASS_TCP) | (1u << PPC_CLASS_ARP);
    ppc_view_set(&v, &s, 7, PPC_SORT_NEWEST, &f);
    check("two classes, the newest first", ppc_view_count(&v) == 50 && ppc_view_at(&v, 0) == 99 &&
                                               ppc_view_at(&v, 1) == 97 && ppc_view_at(&v, 49) == 1);
    check("the view keeps its id", v.id == 7);
    ppc_view_free(&v);
    ppc_store_free(&s);
}

static void test_ring(void)
{
    struct ppc_store s;
    struct ppc_view v;
    struct ppc_filter f;
    uint32_t i;
    int ok = 1;
    int exact = 0;

    /* 64 summaries: the store turns many times over. */
    ppc_store_init(&s, 64, 1u << 16);
    ppc_view_init(&v, 64);
    memset(&f, 0, sizeof(f));
    f.classes = PPC_CLASSES_ALL;
    ppc_view_set(&v, &s, 1, PPC_SORT_TIME, &f);
    for (i = 0; i < 10; i++) {
        add(&s, i);
    }
    ppc_view_update(&v, &s);
    check("new packets join the view on update", ppc_view_count(&v) == 10 && ppc_view_at(&v, 9) == 10);
    for (i = 10; i < 1000; i++) {
        add(&s, i);
        if (i % 37 == 0) {
            ppc_view_update(&v, &s);
            ok = ok && ppc_view_count(&v) == ppc_store_count(&s) && ppc_view_at(&v, 0) == s.first_seq &&
                 ascending(&v);
        }
    }
    ppc_view_update(&v, &s);
    check("as the store turns: exactly what it holds, oldest first, never twice",
          ok && ppc_view_count(&v) == 64 && ppc_view_at(&v, 0) == s.first_seq && ppc_view_at(&v, 63) == s.last_seq);
    check("rank of a held seq", ppc_view_rank_of(&v, s.first_seq + 5, &exact) == 5 && exact);
    check("rank of a dropped seq: the next one held", ppc_view_rank_of(&v, 3, &exact) == 0 && !exact);
    check("rank past the newest: the newest", ppc_view_rank_of(&v, s.last_seq + 50, &exact) == 63 && !exact);
    check("rank of a time: the first packet at or after it",
          ppc_store_get(&s, ppc_view_at(&v, ppc_view_rank_of_time(&v, &s, 9500ull * 1000000u)))->rel_ns >=
              9500ull * 1000000u);
    f.classes = 1u << PPC_CLASS_TCP;
    ppc_view_set(&v, &s, 2, PPC_SORT_NEWEST, &f);
    check("newest first: a seq missing from the view gives the nearest earlier one",
          ppc_view_at(&v, ppc_view_rank_of(&v, s.last_seq - 1, &exact)) < s.last_seq && !exact);
    check("newest first: a time gives that second's newest packet",
          ppc_store_get(&s, ppc_view_at(&v, ppc_view_rank_of_time(&v, &s, 9500ull * 1000000u)))->rel_ns <
              10500ull * 1000000u);
    ppc_view_free(&v);
    ppc_store_free(&s);
}

static void test_len_order(void)
{
    struct ppc_store s;
    struct ppc_view v;
    struct ppc_filter f;
    uint32_t i;
    int ok = 1;

    ppc_store_init(&s, 1024, 1u << 16);
    ppc_view_init(&v, 1024);
    for (i = 0; i < 70; i++) {
        add(&s, i);
    }
    memset(&f, 0, sizeof(f));
    f.classes = PPC_CLASSES_ALL;
    ppc_view_set(&v, &s, 1, PPC_SORT_LEN, &f);
    ppc_view_prepare(&v, &s, 5000);
    for (i = 1; i < ppc_view_count(&v); i++) {
        const struct ppc_record *a = ppc_store_get(&s, ppc_view_at(&v, i - 1));
        const struct ppc_record *b = ppc_store_get(&s, ppc_view_at(&v, i));

        ok = ok && (a->wire_len > b->wire_len || (a->wire_len == b->wire_len && a->seq < b->seq));
    }
    check("length order: the largest first, ties in capture order", ok && ppc_view_count(&v) == 70 &&
                                                                         ppc_store_get(&s, ppc_view_at(&v, 0))->wire_len ==
                                                                             660);
    add(&s, 6 + 7 * 100); /* 660 bytes again, the newest */
    ppc_view_update(&v, &s);
    ppc_view_prepare(&v, &s, 5500);
    check("not rebuilt within a second of the last build", ppc_view_count(&v) == 70);
    ppc_view_prepare(&v, &s, 6000);
    check("rebuilt after it, with the new packet among the largest", ppc_view_count(&v) == 71 &&
                                                                         ppc_view_at(&v, 10) == 71);
    check("the rank of a seq in the length order", ppc_view_rank_of(&v, 71, NULL) == 10);
    ppc_view_free(&v);
    ppc_store_free(&s);
}

static void test_pins(void)
{
    struct ppc_store s;
    struct ppc_view v;
    struct ppc_filter f;
    const struct ppc_pin *pin = NULL;
    uint32_t i;

    ppc_store_init(&s, 64, 1u << 16);
    ppc_view_init(&v, 64);
    for (i = 0; i < 20; i++) {
        add(&s, i);
    }
    ppc_store_pin(&s, 12);
    ppc_store_pin(&s, 3);
    ppc_store_pin(&s, 17);
    memset(&f, 0, sizeof(f));
    f.classes = PPC_CLASSES_ALL;
    f.pinned = 1;
    ppc_view_set(&v, &s, 1, PPC_SORT_TIME, &f);
    ppc_view_prepare(&v, &s, 0);
    check("the pinned view: its packets in capture order", ppc_view_count(&v) == 3 && ppc_view_at(&v, 0) == 3 &&
                                                               ppc_view_at(&v, 1) == 12 && ppc_view_at(&v, 2) == 17);
    for (i = 20; i < 400; i++) {
        add(&s, i);
    }
    ppc_view_update(&v, &s);
    ppc_view_prepare(&v, &s, 0);
    check("pins outlive the store: still three after it turned over",
          ppc_view_count(&v) == 3 && ppc_store_get(&s, 3) == NULL && ppc_view_record(&s, 3, &pin) != NULL && pin &&
              pin->seq == 3);
    f.classes = 1u << PPC_CLASS_TCP;
    ppc_view_set(&v, &s, 2, PPC_SORT_NEWEST, &f);
    ppc_view_prepare(&v, &s, 0);
    check("pinned and TCP, the newest first", ppc_view_count(&v) == 1 && ppc_view_at(&v, 0) == 17);
    check("a seq neither held nor pinned is gone", ppc_view_record(&s, 5, &pin) == NULL && pin == NULL);
    ppc_store_unpin(&s, 17);
    ppc_view_prepare(&v, &s, 0);
    check("unpinned: out of the pinned view", ppc_view_count(&v) == 0);
    ppc_view_free(&v);
    ppc_store_free(&s);
}

int main(void)
{
    struct ppc_view v;

    setvbuf(stdout, NULL, _IOLBF, 0);
    test_host_words();
    test_filter();
    test_ring();
    test_len_order();
    test_pins();
    check("a capacity that is not a power of two is refused", ppc_view_init(&v, 1000) != 0);
    printf("ppc_view_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}

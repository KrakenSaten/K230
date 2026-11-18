/*
 * pocketpcap view: the packet list's display filter and sort, kept by the
 * capture helper next to its store (docs/apps/PACKETSHARK.md §7; DS §59.4:
 * a window, not a scroll). Milestone M5.
 *
 * A view is the ordered list of the stored packets that pass the display
 * filter. The shell never sees the list; it asks for windows of it by rank
 * (a position in the view's order), by sequence number or by time, and gets
 * at most PPC_WINDOW_MAX rows of text back.
 *
 * How it is kept:
 *   - an index ring of the matching sequence numbers in capture order,
 *     extended by ppc_view_update() with the packets stored since the last
 *     call and trimmed of those the store has dropped. One check per new
 *     packet: the view costs nothing while no window is asked for, and
 *     nothing at all in the capture path;
 *   - for the length sort, a snapshot of the ring ordered by wire length
 *     (largest first, then capture order), built by a two-pass radix sort in
 *     linear time, and rebuilt no more than once a second while packets keep
 *     arriving (ppc_view_prepare());
 *   - for the pinned view, the store's pins (at most PPC_PINS), which may
 *     have left the rings: a pin outlives its packet there (D9).
 *
 * Both arrays are allocated once, by ppc_view_init() at START, and never
 * grown. Nothing here formats text, blocks or knows about time beyond the
 * clock the caller passes in.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PPC_VIEW_H
#define PPC_VIEW_H

#include "ppc_proto.h"
#include "ppc_record.h"
#include "ppc_store.h"

#include <stddef.h>
#include <stdint.h>

#define PPC_VIEW_RESORT_MS 1000u

/* The display filter: every criterion that is set must hold. */
struct ppc_filter {
    uint8_t classes;   /* bit per enum ppc_class; PPC_CLASSES_ALL or 0: every class */
    uint8_t host_len;  /* 0: any host; 4 IPv4, 16 IPv6, 6 MAC */
    uint8_t host[16];  /* either end of the packet */
    uint16_t port;     /* 0: any port; else either end's TCP or UDP port */
    uint8_t pinned;    /* only pinned packets */
    uint8_t signals;   /* PPC_SIG_* bits; 0: any packet, else one with any of these findings (M7) */
    uint8_t diag;      /* enum ppc_diag_code; 0: any packet, else only packets with that diagnosis (M11.1) */
};

enum ppc_sort {
    PPC_SORT_TIME = 0, /* capture order, oldest first: the newest at the end */
    PPC_SORT_NEWEST,   /* capture order reversed: the newest first */
    PPC_SORT_LEN,      /* wire length, largest first; ties in capture order */
    PPC_SORT_COUNT
};

struct ppc_view {
    struct ppc_filter f;
    enum ppc_sort sort;
    uint32_t id;           /* the shell's name for this filter and sort */
    /* the ring of matching seqs, ascending */
    uint32_t *idx;
    uint32_t cap;          /* a power of two, at least the store's summaries */
    uint32_t head;         /* slot of the oldest entry */
    uint32_t count;
    uint32_t scanned;      /* the newest seq already checked */
    /* the length order */
    uint32_t *sorted;
    uint32_t *tmp;
    uint32_t nsorted;
    int sorted_valid;
    int dirty;             /* the ring changed since the sorted snapshot */
    uint64_t sorted_ms;
    /* the pinned view: up to PPC_PINS seqs, in this view's order */
    uint32_t pins[PPC_PINS];
    uint32_t npins;
};

/* An address as the shell sends it: IPv4 dotted, IPv6, or a MAC with
 * colons. Returns its length (4, 16 or 6), or -1. */
int ppc_filter_host(const char *text, uint8_t out[16]);

/* Whether r passes f. pinned is not checked here (the caller knows). */
int ppc_filter_match(const struct ppc_filter *f, const struct ppc_record *r);

/* "time", "newest", "len"; -1 for anything else. */
int ppc_sort_parse(const char *word);
const char *ppc_sort_name(enum ppc_sort s);

/* Allocate for a store of `cap` summaries (a power of two). 0, or -1 with
 * nothing held. The view starts as every packet in capture order, id 0. */
int ppc_view_init(struct ppc_view *v, uint32_t cap);
void ppc_view_free(struct ppc_view *v);

/* A new filter and sort: the ring is rebuilt from the store now (one check
 * per stored packet, at most cap). */
void ppc_view_set(struct ppc_view *v, const struct ppc_store *s, uint32_t id, enum ppc_sort sort,
                  const struct ppc_filter *f);

/* Take in what the store gained and lost since the last call. Cheap: one
 * check per new packet. */
void ppc_view_update(struct ppc_view *v, const struct ppc_store *s);

/* Make the order ready for a window: for the length sort, rebuild the
 * snapshot when it was never built, or when the ring changed and the last
 * build is PPC_VIEW_RESORT_MS old; for the pinned view, read the pins. */
void ppc_view_prepare(struct ppc_view *v, const struct ppc_store *s, uint64_t now_ms);

/* Packets in the view (for the length sort: in its snapshot). */
uint32_t ppc_view_count(const struct ppc_view *v);

/* The seq at rank (0 .. count-1), or 0 outside. A seq from the length
 * snapshot or the pins may have left the store since. */
uint32_t ppc_view_at(const struct ppc_view *v, uint32_t rank);

/* The rank of seq. exact is set when seq itself is in the view. When it is
 * not, the capture orders give the rank of the next packet in their order
 * (the nearest later one for TIME, the nearest earlier one for NEWEST), so
 * a list held at a packet that left the store stays where it was; the
 * length order and the pins give 0. count when the view is empty. */
uint32_t ppc_view_rank_of(const struct ppc_view *v, uint32_t seq, int *exact);

/* The rank of the first packet at or after rel_ns, in the capture orders
 * (for NEWEST: the last packet at or before it, so the list opens on that
 * second); 0 for the others. */
uint32_t ppc_view_rank_of_time(const struct ppc_view *v, const struct ppc_store *s, uint64_t rel_ns);

/* The record of seq from the store, or from its pin when it left the store;
 * NULL when neither has it. *pin is set to the pin when the record came
 * from one (its bytes are the pin's), else NULL. */
const struct ppc_record *ppc_view_record(const struct ppc_store *s, uint32_t seq, const struct ppc_pin **pin);

#endif

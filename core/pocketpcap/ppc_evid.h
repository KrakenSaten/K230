/*
 * pocketpcap evidence history (N1.0; docs/apps/PACKETSHARK.md §4d): what the
 * capture observed around a moment the person marked.
 *
 *   observations  one per relevant packet: a DHCP message, an ARP request,
 *                 reply, probe or announcement, an address changing hands,
 *                 a DNS query or answer. A fact a packet stated, with the
 *                 packet's number, never a conclusion drawn from it.
 *   history       the last PPC_EV_HISTORY observations, in a ring.
 *   marks         at most PPC_EV_MARKS moments. A mark owns its evidence:
 *                 the 60 s before it, copied out of the history when it is
 *                 set, and the 60 s after it, gathered as they pass. Each
 *                 side has every observation counted by kind, the rows kept
 *                 to show, and how much of the window the capture watched.
 *   visibility    per second: watching, on HOLD, shedding, frames lost;
 *                 and for the whole capture, the filter the source applied
 *                 and the classes whose analysis could not start.
 *
 * An absent observation is never turned into a fact here. Whether a reply
 * could have been seen at all travels with the request (PPC_EVF_REPLY_HERE);
 * what a window lacks is told by its visibility, not guessed.
 *
 * Every frame counts once: frames lost before a mark are in the window
 * before it, after it in the window after, and those that cannot be placed
 * on either side (the counters could not be read at the mark) are told once,
 * apart, as lost around the mark.
 *
 * Time is the session's own: milliseconds since START, given by the caller.
 * One allocation, at init. No text, no clock, no socket, no loop without a
 * bound (tests/ppc_evid_test.c; tests/packetshark_lint.sh).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PPC_EVID_H
#define PPC_EVID_H

#include "ppc_proto.h"

#include <stdint.h>

#define PPC_EV_HISTORY 2048u /* observations remembered, a power of two */
#define PPC_EV_MARKS 8u
#define PPC_EV_PLAIN_KEEP 3u /* of the PPC_EV_KEEP rows a side keeps, routine ARP at most */
#define PPC_EV_SECONDS 64u   /* seconds of visibility remembered, a power of two */
/* When the counters of lost frames could not be read at a mark, what they
 * report within this long after it may have been lost before it or after:
 * the helper reads them every 250 ms and the history learns of a reading at
 * its next tick, 250 ms later at most. */
#define PPC_EV_EDGE_MS 1000u

/* An observation's kind (enum ppc_ev_kind) and flags (PPC_EVF_*), a
 * window's flags (PPC_EVW_*), its length and the rows a side keeps are part
 * of the protocol: ppc_proto.h. */

struct ppc_ev_obs {
    uint32_t t_ms;   /* since START */
    uint32_t seq;    /* its packet */
    uint8_t kind;    /* enum ppc_ev_kind */
    uint8_t flags;   /* PPC_EVF_* */
    uint8_t mac[6];  /* the client, the asker or the sender; zero: none */
    uint8_t ip[4];   /* DHCP: the address asked for or given; ARP: the sender's; DNS: the resolver */
    uint8_t ip2[4];  /* DHCP: the server named; ARP: the address asked about; DNS: the client */
    uint32_t a;      /* DHCP: transaction id; DNS: id, the response code above it (<< 16) */
    uint32_t b;      /* DHCP ACK: lease seconds; DNS answer: response time in microseconds */
};

_Static_assert(sizeof(struct ppc_ev_obs) == 32, "struct ppc_ev_obs is 32 bytes");

/* One side of a mark: the window before it (0) or after it (1). */
struct ppc_ev_side {
    uint32_t count[PPC_EV_KINDS]; /* every observation of the window, by kind */
    uint32_t nkept;
    uint32_t nplain;              /* of the kept rows, the routine ones */
    uint32_t more;                /* observations counted and not kept as rows */
    uint32_t span_ms;             /* how much of the 60 s the window covers */
    uint32_t watched_s;           /* seconds the capture watched */
    uint32_t held_s;              /* seconds on HOLD */
    uint32_t shed_s;              /* seconds frames were counted and not judged */
    uint64_t lost;                /* frames the kernel or the interface reported lost */
    uint8_t flags;                /* PPC_EVW_* */
    struct ppc_ev_obs kept[PPC_EV_KEEP]; /* oldest first */
};

struct ppc_ev_mark {
    uint32_t id;     /* 1-based, never reused in a session; 0: free */
    uint32_t at_ms;  /* since START */
    uint32_t seq;    /* the newest packet when it was set; 0: none yet */
    uint8_t state;   /* what the capture was doing (enum ppc_state) */
    uint8_t exact;   /* the lost-frame counters were read at the mark */
    uint64_t wall_s; /* the wall clock at the mark, after STOP at STOP; 0: not set */
    uint32_t lost_split; /* frames lost in the mark's second up to the mark */
    uint64_t edge_lost;  /* frames lost around the mark, in neither window */
    struct ppc_ev_side side[2];
};

/* What a second was. */
#define PPC_EVS_WATCHED 0x01u
#define PPC_EVS_HELD 0x02u
#define PPC_EVS_SHED 0x04u

struct ppc_ev_sec {
    uint32_t sec;    /* since START */
    uint32_t lost;
    uint8_t flags;   /* PPC_EVS_*; 0: not filled */
    uint8_t used;
};

struct ppc_evid {
    void *mem;       /* the one allocation */
    struct ppc_ev_obs *hist;
    uint32_t head;   /* the next place in the ring */
    uint32_t count;  /* observations in it */
    uint64_t total;  /* observations ever taken */
    uint32_t gone_ms; /* when the newest observation that left the ring was made */
    uint8_t gone;    /* one has left */
    struct ppc_ev_mark *marks; /* PPC_EV_MARKS, a ring by id */
    uint32_t next_id;
    struct ppc_ev_sec secs[PPC_EV_SECONDS];
    uint32_t cur_sec;
    uint8_t ticking; /* a tick has been taken */
    uint8_t cur_flags;
    uint8_t stopped;
    uint32_t stop_ms;
    uint64_t stop_wall_s;
    uint64_t lost_total;
    uint8_t cfilter;    /* enum ppc_cfilter the source applied */
    uint8_t unanalyzed; /* PPC_EV_CLASSES whose analysis could not start */
    uint8_t bypass_any; /* a frame has bypassed analysis */
    uint32_t bypass_sec; /* the second it was in */
};

/* Allocate the history and the marks. 0, or -1 with nothing held. */
int ppc_evid_init(struct ppc_evid *ev);
void ppc_evid_free(struct ppc_evid *ev);

/* What the whole capture can observe: the capture filter its source
 * applied (enum ppc_cfilter; a replay or the synthetic source applies none)
 * and the classes whose analysis could not start (PPC_EV_CLASSES). Given
 * once, at START. An observation of a class either keeps out is not taken. */
void ppc_evid_coverage(struct ppc_evid *ev, unsigned cfilter, unsigned unanalyzed);
/* The classes whose analysis could not start: DNS without the DNS table
 * (ppc_signals), DHCP and ARP without the network's tables (ppc_net). */
uint8_t ppc_evid_unanalyzed(int dns_ready, int net_ready);

/* One observation, in the order the packets came. */
void ppc_evid_note(struct ppc_evid *ev, const struct ppc_ev_obs *o);

/* A frame stamped t_ms passed without being analysed (the capture fell
 * behind and only counts): its second was not watched whole, however short
 * the burst, and between ticks or not. Called for every such frame; the
 * second is written once. */
void ppc_evid_bypass(struct ppc_evid *ev, uint32_t t_ms);

/* The session's clock and what the capture is doing (PPC_EVS_*: one of
 * them), at least once a second while a capture runs; lost_total is the
 * running count of frames reported lost. */
void ppc_evid_tick(struct ppc_evid *ev, uint32_t now_ms, unsigned flags, uint64_t lost_total);

/* The capture stopped: windows still filling are closed here. wall_s is
 * the wall clock then (0: not set), which a mark set later takes. */
void ppc_evid_stop(struct ppc_evid *ev, uint32_t now_ms, uint64_t wall_s);

/* Set a mark now. Never fails once init has: the oldest mark is replaced
 * when all are taken. state is the capture's (enum ppc_state), last_seq the
 * newest packet's number, wall_s the wall clock now (0: not set). After
 * STOP the mark stands where the capture stopped, on both clocks. exact:
 * the lost_total of the tick just given was read at the mark; when not,
 * the frames reported lost in the PPC_EV_EDGE_MS after it are told apart. */
const struct ppc_ev_mark *ppc_evid_mark(struct ppc_evid *ev, uint32_t now_ms, unsigned state, uint32_t last_seq,
                                        uint64_t wall_s, int exact);

/* The mark of an id, or the newest for id 0; NULL when it is not held. */
const struct ppc_ev_mark *ppc_evid_get(const struct ppc_evid *ev, uint32_t id);
/* How many marks are held, and the oldest and newest id. */
unsigned ppc_evid_ids(const struct ppc_evid *ev, uint32_t *first, uint32_t *last);

/* The bytes of the one allocation, for the bench and the docs. */
uint32_t ppc_evid_bytes(void);

/* ---- from the frames ----------------------------------------------------------------------
 *
 * The observation a frame makes, built from what the network intelligence
 * said of it (ppc_net_listen) or from a DNS record and its diagnosis. Each
 * returns 1 with *o filled, or 0 when the frame is not an observation.
 * lead_ms is the session time of the first packet: a record's rel_ns counts
 * from that packet, an observation's t_ms from START. self is this device's
 * MAC, NULL while it is not known (nothing it sent has been seen). */
struct ppc_net_said;
struct ppc_record;

int ppc_evid_from_net(const struct ppc_net_said *s, uint32_t lead_ms, const uint8_t *self, struct ppc_ev_obs *o);
/* r: the DNS record; diag: its diagnosis (enum ppc_diag_code); rtt_us: the
 * response time of an answer, 0 for none; to_self: the frame was addressed
 * to this device. */
int ppc_evid_from_dns(const struct ppc_record *r, unsigned diag, uint32_t rtt_us, int to_self, uint32_t lead_ms,
                      uint32_t seq, struct ppc_ev_obs *o);

#endif

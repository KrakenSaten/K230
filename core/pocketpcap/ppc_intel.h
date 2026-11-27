/*
 * pocketpcap intelligence engine, first slice (N1.1;
 * docs/apps/packetshark/neural/N1.1.md): deterministic before/after
 * comparisons around a mark, on packet time, that say how much of each
 * window was seen and abstain, with a reason, when what was lost could
 * explain the difference.
 *
 *   series      one record per second of packet time (PPC_IN_SECONDS, a
 *               ring): what the capture was doing (watching, HOLD,
 *               shedding), frames analysed and shed, this device's frames
 *               in and out, its DNS questions, answers, failures and
 *               retries, DHCP DISCOVERs and ARP requests.
 *   questions   this device's DNS questions (PPC_IN_QUESTIONS, a ring),
 *               each with its packet, its answer's packet and the answer's
 *               time; a question is decided only once PPC_IN_WAIT_MS of
 *               watched capture followed it.
 *   readings    what the lost-frame counters told, as intervals between
 *               two good readings (PPC_IN_READINGS, a ring of the intervals
 *               in which frames were lost; the rest is known from the last
 *               good reading); a spell of failed readings stays open, and
 *               unknown, until a good reading closes it.
 *   ledger      the comparisons made final (PPC_IN_LEDGER, a ring, ids
 *               never reused): for a mark, the minute before it against
 *               the two minutes before that (pair A), and the minute after
 *               it against the minute before (pair B), metric by metric.
 *
 * It is fed what the evidence history (ppc_evid.h, N1.0) is fed: the same
 * observations, the same ticks and counter readings, the same marks. Its
 * clock is the session's (milliseconds since START), which a frame gives as
 * its packet time; a replay that tells it the same frames, readings and
 * marks at the same times gets the same ledger however often it is ticked.
 *
 * A loss bound is an interval, never a guess: frames reported by a reading
 * whose interval lies inside a window are surely the window's (lo); those of
 * a reading that straddles its edge may be (hi); losses not known bound it
 * by 1. Each test runs at both ends of the bound. A change is claimed only
 * when the end that favours no change rejects; when only the other end
 * does, it abstains (PPC_INR_LOSS_EXPLAINS, or PPC_INR_LOSS_UNKNOWN). "No
 * change" is said only when the window could also rule out a material
 * worsening (PPC_IN_MATERIAL_*), otherwise PPC_INR_WEAK; a significant
 * answer-time difference below the practical bound is PPC_INV_SMALL, never
 * "no change". Loss is assumed to strike frames independently of their kind
 * (ASSUMED, documented in N1.1.md).
 *
 * Answer times are guarded against selective loss: the answers not seen (a
 * question decided unanswered: lost, or slower than the wait) are counted as
 * the slowest, and every verdict must survive that (PPC_INR_UNSEEN when only
 * the answers seen support it). A question is decided unanswered only when
 * its wait was watched whole and some frame other than a DNS answer reached
 * this device during it: the way back was seen working.
 *
 * One-sided visibility (this device sends and nothing comes back) is told
 * from its own frames in and out: a window in which nothing came back, or a
 * silent run its inbound rate makes implausible. DNS comparisons then
 * abstain (PPC_INR_ONE_WAY); a resolver that stopped and replies taking
 * another path look the same from here.
 *
 * Kept: counts, times and packet numbers. No address, name or byte of a
 * packet. One allocation, at init; no text, no clock, no socket, no loop
 * without a bound (tests/ppc_intel_test.c; tests/packetshark_lint.sh).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PPC_INTEL_H
#define PPC_INTEL_H

#include "ppc_evid.h"

#include <stdint.h>

#define PPC_IN_SECONDS 512u    /* seconds of series kept, a power of two */
#define PPC_IN_QUESTIONS 512u  /* this device's DNS questions kept, a power of two */
#define PPC_IN_EVENTS 256u     /* DHCP DISCOVER and ARP request packets kept for references, a power of two */
#define PPC_IN_READINGS 512u   /* loss intervals kept, a power of two */
#define PPC_IN_LEDGER 128u     /* comparisons kept, a power of two */
#define PPC_IN_MARKS PPC_EV_MARKS
#define PPC_IN_REFS 4u
#define PPC_IN_WAIT_MS 5000u   /* a question is unanswered only after this much watched capture */
#define PPC_IN_WINDOW_MS PPC_EV_WINDOW_MS
#define PPC_IN_REF_MS (2u * PPC_EV_WINDOW_MS) /* pair A's reference: the two minutes before the minute before */

/* What is compared. */
enum ppc_in_metric {
    PPC_INM_DNS_TIME = 0,  /* this device's DNS answer time: medians, Mann-Whitney */
    PPC_INM_DNS_ANSWERED,  /* this device's DNS questions answered: binomial, loss-aware */
    PPC_INM_DNS_FAILED,    /* answers that carried a failure code: binomial */
    PPC_INM_DHCP_DISCOVER, /* DHCP DISCOVERs from any device, a minute: Poisson, loss-aware */
    PPC_INM_ARP_REQUEST,   /* ARP requests from any device, a minute: Poisson, loss-aware */
    PPC_INM_COUNT
};

/* What a comparison found. A change is said in the harmful direction's
 * terms: slower, fewer answered, more failures, more DISCOVERs or ARP. */
enum ppc_in_verdict {
    PPC_INV_PENDING = 0,
    PPC_INV_NO_CHANGE,    /* the test could see a change of this size, and did not */
    PPC_INV_WORSE,
    PPC_INV_BETTER,
    PPC_INV_CANT_TELL,    /* with a reason */
    PPC_INV_SMALL,        /* a significant difference below the practical bound (answer time only) */
};

/* How good the evidence under it is. */
enum ppc_in_quality {
    PPC_INQ_GOOD = 0,     /* both windows watched whole, nothing lost */
    PPC_INQ_DEGRADED,     /* frames lost, or seconds not watched, and the test still decided */
    PPC_INQ_INSUFFICIENT, /* it abstained */
};

/* Why it abstained, the first that applies, in this order. */
enum ppc_in_reason {
    PPC_INR_NONE = 0,
    PPC_INR_NOT_CAPTURED,  /* the capture filter kept the protocol out */
    PPC_INR_NOT_ANALYZED,  /* its analysis could not start */
    PPC_INR_CUT,           /* a window lies (mostly) before the capture started */
    PPC_INR_HISTORY,       /* the series or the readings no longer hold a window */
    PPC_INR_NOT_WATCHED,   /* less than half of a window was watched (HOLD, shedding) */
    PPC_INR_ONE_WAY,       /* this device sent and nothing came back to it: one-sided visibility */
    PPC_INR_LOSS_UNKNOWN,  /* the counters could not be read over a window */
    PPC_INR_TOO_FEW,       /* fewer than the minimum to compare */
    PPC_INR_LOSS_EXPLAINS, /* only the loss bound's end that favours a change rejects: the frames lost
                            * could account for the change, or hide one */
    PPC_INR_WEAK,          /* nothing rejected, and too little evidence to rule out a material worsening */
    PPC_INR_UNSEEN,        /* answer times: the answers not seen could undo the difference the seen ones show */
    PPC_INR_COUNT
};

/* Notes beside a verdict. */
#define PPC_INN_UNWATCHED 0x01u  /* seconds not watched: when a change happened is not known to the second */
#define PPC_INN_LOSS 0x02u       /* frames were (or may have been) lost in a window */
#define PPC_INN_UNDECIDED 0x04u  /* questions left out: their wait was not watched whole */
#define PPC_INN_RETRIES 0x08u    /* questions were asked again */
#define PPC_INN_STOPPED 0x10u    /* the capture stopped inside the window after */
#define PPC_INN_UNSEEN 0x20u     /* answer times: answers not seen were counted as the slowest */
#define PPC_INN_ALL 0x3fu

#define PPC_IN_MIN_PAIRS 5u      /* answer times a window needs */
#define PPC_IN_MIN_QUESTIONS 5u  /* decided questions a window needs */
#define PPC_IN_MIN_ANSWERS 5u    /* answers a window needs for the failure share */
#define PPC_IN_MIN_WATCHED_S 10u /* watched seconds a window needs for a rate */
#define PPC_IN_ALPHA_E6 10000u   /* one-sided test level, x 1e6 (0.01) */
#define PPC_IN_SLOWER_PCT 150u   /* a slower median is a change only from 1.5 times ... */
#define PPC_IN_SLOWER_US 20000u  /* ... and 20 ms more */
/* What "no change" must rule out (ASSUMED design values, N1.1.md): */
#define PPC_IN_MATERIAL_SHARE_PCT 50u /* the answered share halving */
#define PPC_IN_MATERIAL_FAIL_PCT 25u  /* the failure share 25 points higher */
#define PPC_IN_MATERIAL_RATE 3.0      /* a rate three times the base's ... */
#define PPC_IN_MATERIAL_RATE_MIN 6.0  /* ... or 6 a minute more, whichever is more */

/* How much of a window the capture saw. */
struct ppc_in_vis {
    uint32_t from_ms, to_ms; /* the window, clipped to the capture */
    uint16_t watched_s, held_s, shed_s, gap_s; /* gap: seconds the series no longer holds */
    uint32_t frames;         /* analysed or shed */
    uint32_t lost_lo, lost_hi;
    uint8_t loss_known;      /* no open spell of failed readings overlaps it */
    uint32_t own_in, own_out;
};

/* One comparison: a window against its base. Index 0 is the base, 1 the
 * window compared with it. */
struct ppc_in_cmp {
    uint32_t id;          /* 1-based, never reused; 0: free */
    uint32_t mark_id;     /* the N1.0 mark it belongs to */
    uint8_t pair;         /* 0: before against its reference; 1: after against before */
    uint8_t metric;       /* enum ppc_in_metric */
    uint8_t verdict;      /* enum ppc_in_verdict */
    uint8_t quality;      /* enum ppc_in_quality */
    uint8_t reason;       /* enum ppc_in_reason */
    uint8_t notes;        /* PPC_INN_* */
    uint32_t n[2];        /* pairs, decided questions, answers, or watched seconds */
    uint32_t k[2];        /* -, answered, failed, events */
    uint32_t stat[2];     /* median answer time in us; share in permille; events a minute x 100 */
    uint32_t undecided[2];
    uint32_t p_e6;        /* the deciding test's p-value x 1e6, 1000000 when none ran */
    uint32_t refs[PPC_IN_REFS]; /* packets in the window compared: the slowest answers, the questions not
                                 * answered, the failures, the first events; 0: none */
    struct ppc_in_vis vis[2];
};

struct ppc_in_sec {
    uint32_t sec;
    uint8_t used;
    uint8_t flags;        /* PPC_EVS_* seen in the second, OR-ed */
    uint16_t dns_q, dns_retry, dns_ans, dns_fail, dhcp_disc, arp_req;
    uint32_t frames;
    uint32_t own_in, own_out;
};

struct ppc_in_q {
    uint32_t t_ms;
    uint32_t seq;         /* the question's packet */
    uint32_t ans_seq;     /* its answer's; 0: none seen */
    uint32_t rtt_us;
    uint16_t id;
    uint8_t answered;     /* 1 answered, 2 answered with a failure code */
    uint8_t retries;
};

struct ppc_in_evt {
    uint32_t t_ms;
    uint32_t seq;
    uint8_t metric;       /* PPC_INM_DHCP_DISCOVER or PPC_INM_ARP_REQUEST */
};

struct ppc_in_read {
    uint32_t from_ms, to_ms; /* (from, to]: what the reading at to_ms told since the good one at from_ms */
    uint32_t lost;
};

struct ppc_in_mark {
    uint32_t id;          /* the N1.0 mark's; 0: free */
    uint32_t at_ms;
    uint8_t done[2];      /* pair A, pair B made final */
};

struct ppc_intel {
    void *mem;
    struct ppc_in_sec *secs;
    struct ppc_in_q *qs;
    struct ppc_in_evt *evts;
    struct ppc_in_read *reads;
    struct ppc_in_cmp *ledger;
    struct ppc_in_mark marks[PPC_IN_MARKS];
    uint32_t nq, nevt, nread, nledger, next_id, mark_slot;
    uint32_t now_ms;
    uint32_t cur_sec;     /* the newest second filled */
    uint8_t ticking;
    uint8_t cur_flags;    /* PPC_EVS_* the capture is in */
    uint8_t stopped;
    uint32_t stop_ms;
    uint32_t read_ms;     /* the last good reading */
    uint64_t lost_total;  /* as it told it */
    uint8_t failing;      /* a spell of failed readings is open since read_ms */
    uint32_t reads_from_ms; /* the oldest moment the readings still cover */
    uint8_t cfilter;
    uint8_t unanalyzed;
};

int ppc_intel_init(struct ppc_intel *in);
void ppc_intel_free(struct ppc_intel *in);
/* As ppc_evid_coverage(). */
void ppc_intel_coverage(struct ppc_intel *in, unsigned cfilter, unsigned unanalyzed);

/* A frame at t_ms, analysed (shed 0) or only counted (shed 1); pkttype as
 * the source told it (PPC_PKT_*). Every frame the helper hands on, in order. */
void ppc_intel_frame(struct ppc_intel *in, uint32_t t_ms, unsigned pkttype, int shed);
/* An observation, as given to ppc_evid_note(). */
void ppc_intel_note(struct ppc_intel *in, const struct ppc_ev_obs *o);
/* As ppc_evid_tick(), ppc_evid_counters() and ppc_evid_stop(). A tick also
 * makes final what has become final. */
void ppc_intel_tick(struct ppc_intel *in, uint32_t now_ms, unsigned flags);
void ppc_intel_counters(struct ppc_intel *in, uint32_t now_ms, int ok, uint64_t lost_total);
void ppc_intel_stop(struct ppc_intel *in, uint32_t now_ms);
/* A mark set by ppc_evid_mark(), with its id and time. */
void ppc_intel_mark(struct ppc_intel *in, uint32_t mark_id, uint32_t at_ms);

/* A comparison as it stands now, made final or not (out->id 0 when not).
 * 0, or -1 when the mark is not held. */
int ppc_intel_compare(const struct ppc_intel *in, uint32_t mark_id, unsigned pair, unsigned metric,
                      struct ppc_in_cmp *out);
/* The ledger: comparisons made final, oldest first; i from 0. NULL past the end. */
const struct ppc_in_cmp *ppc_intel_ledger(const struct ppc_intel *in, uint32_t i);
uint32_t ppc_intel_ledger_count(const struct ppc_intel *in);

uint32_t ppc_intel_bytes(void);

/* What the buffer still holds of a referenced packet (enum ppc_ev_alive). */
struct ppc_store;
unsigned ppc_intel_alive(const struct ppc_store *s, uint32_t seq);

const char *ppc_in_metric_id(unsigned m);
const char *ppc_in_verdict_id(unsigned v);
const char *ppc_in_quality_id(unsigned q);
const char *ppc_in_reason_id(unsigned r);

/* The tests (exposed for tests/ppc_intel_test.c): one-sided tails x 1e6. */
uint32_t ppc_in_binom_le_e6(uint32_t k, uint32_t n, double p);
uint32_t ppc_in_binom_ge_e6(uint32_t k, uint32_t n, double p);
uint32_t ppc_in_poisson_ge_e6(uint32_t c, double mu);
uint32_t ppc_in_poisson_le_e6(uint32_t c, double mu);

#endif

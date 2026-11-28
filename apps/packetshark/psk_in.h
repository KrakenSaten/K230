/*
 * PacketShark's WHEN face, its words (N1.2, DS §59.12): a mark's
 * comparisons as protocol 7 brings them (ppc_proto.h `in.*`), said as
 * observations. The engine's verdicts are carried but not shown as problem
 * claims while automatic verdicts are off (PSK_IN_VERDICTS, observation-only
 * first): each comparison shows what was seen before and after, how well it
 * was seen, and, when it cannot be told, why. Expired packet references say
 * so. Every word here is tested (tests/psk_in_test.c); the shell never links
 * the engine.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PSK_IN_H
#define PSK_IN_H

#include "pocketpcap/ppc_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Automatic problem verdicts (WORSE as SLOWER, FEWER, MORE ...): off in
 * N1.2. The engine passed G0 on synthetic data only; field validation comes
 * first (docs/apps/packetshark/neural/G0.md §8.6). */
#ifndef PSK_IN_VERDICTS
#define PSK_IN_VERDICTS 0
#endif

/* The protocol's numbers (ppc_proto.h in.cmp): the shell's copy of the
 * engine's vocabulary, which it must not link. */
enum { PSK_INM_DNS_TIME = 0, PSK_INM_DNS_ANSWERED, PSK_INM_DNS_FAILED, PSK_INM_DHCP, PSK_INM_ARP };
enum { PSK_INV_PENDING = 0, PSK_INV_NO_CHANGE, PSK_INV_WORSE, PSK_INV_BETTER, PSK_INV_CANT_TELL, PSK_INV_SMALL };
enum { PSK_INQ_GOOD = 0, PSK_INQ_DEGRADED, PSK_INQ_INSUFFICIENT };

/* One mark's answer for one pair, whole. */
struct psk_insight {
    struct ppc_evt_in_head head;
    struct ppc_evt_in_vis vis[2];
    struct ppc_evt_in_cmp cmp[PPC_IN_METRICS];
    uint8_t have;  /* bit per metric received */
    uint8_t nvis;
};

/* "DNS ANSWER TIME" ... "ARP REQUESTS". */
const char *psk_in_metric(unsigned metric);

/* What was seen before and after, in the metric's own figures:
 * "MEDIAN 31 MS \xe2\x86\x92 120 MS", "60 OF 60 \xe2\x86\x92 2 OF 45 QUESTIONS", "158 \xe2\x86\x92 790 A MINUTE". */
const char *psk_in_figures(const struct ppc_evt_in_cmp *c, char *buf, size_t buflen);

/* How well it was seen, and why it cannot be told when it cannot:
 * "WATCHED WHOLE", "WATCHED WITH LIMITS \xc2\xb7 FRAMES LOST", "NOT ENOUGH EVIDENCE \xc2\xb7 THE FRAMES
 * LOST COULD ACCOUNT FOR A CHANGE"; fewer DNS answers seen adds that an answer
 * by another path is not seen here. *limited: anything short of watched whole. */
const char *psk_in_evidence(const struct ppc_evt_in_cmp *c, char *buf, size_t buflen, bool *limited);

/* The verdict's word when automatic verdicts are on, "" otherwise (and for
 * a comparison that is not final). */
const char *psk_in_verdict(const struct ppc_evt_in_cmp *c, bool final);

/* Its packets: "PACKETS #812 WHOLE \xc2\xb7 #830 GONE (EXPIRED)"; "" when it has none.
 * *expired: one or more is gone. */
const char *psk_in_refs(const struct ppc_evt_in_cmp *c, char *buf, size_t buflen, bool *expired);

/* What the capture saw of the two windows: "BEFORE 60 S WATCHED \xc2\xb7 AFTER 39 S
 * WATCHED, 21 S ON HOLD \xc2\xb7 56 FRAMES LOST AFTER". *limited: any limit. */
const char *psk_in_seeing(const struct psk_insight *in, char *buf, size_t buflen, bool *limited);

/* One line for LIVE and the page's head: "5 OBSERVED \xc2\xb7 2 WITH LIMITS",
 * "WINDOW STILL FILLING", "" when nothing is held. */
const char *psk_in_summary(const struct psk_insight *in, char *buf, size_t buflen);

/* The answer is whole: in.begin, both windows and the five comparisons. */
bool psk_in_complete(const struct psk_insight *in);

/* The panel's own lines: while no whole answer is in; its head (the summary
 * and that these are observations only); and pair A, the minute before the
 * mark against the two before it, on one line. */
const char *psk_in_waiting(void);
const char *psk_in_head(const struct psk_insight *in, char *buf, size_t buflen);
const char *psk_in_pair_a(const struct psk_insight *in, char *buf, size_t buflen);

#endif

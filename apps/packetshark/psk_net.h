/*
 * PacketShark's network intelligence in words (M12.1,
 * docs/apps/PACKETSHARK.md §4c): what the screens say of the devices, DHCP
 * and ARP the helper counted (core/pocketpcap/ppc_net.h), and of each event
 * it kept.
 *
 * The rule of every sentence here: say what was observed, then what it may
 * mean, and never the second as if it were the first. An observation from
 * one interface is not a diagnosis of the network; a missing answer is not
 * a failure; a warning is "worth a look", and says what else it could be.
 *
 * Pure C over the model, no LVGL, no allocation (tests/psk_net_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PSK_NET_H
#define PSK_NET_H

#include "psk_model.h"

#include <stddef.h>

/* psk_diag_summary(), psk_diag_lines() and psk_diag_confidence() pass the
 * kinds past PSK_DIAG_TCP on to these.
 *
 *   PSK_DIAG_DHCP   "2 SERVERS · 1 NO OFFER · 12 OF 14 ACKNOWLEDGED"
 *   PSK_DIAG_ARP    "2 DUPLICATE CLAIMS · 14 ADDRESSES · 310 REQUESTS"
 *   PSK_DIAG_NET    "2 TO CHECK · 14 DEVICES · 1 DHCP SERVER"
 *
 * The lines of DHCP: the exchanges, each server and what it gives, the
 * newest leases, then what was refused, declined or left unanswered. Of
 * ARP: the pairs and the counts, then conflicts, duplicates and moves. */
unsigned psk_net_summary(const struct psk_model *m, unsigned kind, char *buf, size_t buflen);
unsigned psk_net_lines(const struct psk_model *m, unsigned kind, struct psk_diag_line *out, unsigned max);
const char *psk_net_confidence(const struct psk_model *m, unsigned kind, char *buf, size_t buflen);

/* One event (PSK_DIAG_FINDING). Its title ("TWO MACS CLAIM ONE ADDRESS"),
 * and its sheet: what was observed, what it may mean, and the evidence - the
 * packet that showed it (OPEN) and the packets around it (LIST). */
const char *psk_net_event_title(unsigned kind);
unsigned psk_net_event_lines(const struct ppc_evt_net_event *e, struct psk_diag_line *out, unsigned max);
/* The event the model holds now that is the same finding as `e` (an older
 * copy of it), or NULL when it has left the helper's ring. */
const struct ppc_evt_net_event *psk_net_event_same(const struct psk_model *m, const struct ppc_evt_net_event *e);

/* The findings of the whole capture in one list: the helper's DHCP and ARP
 * events, and the lines of the DNS and TCP diagnosis that are worth a look.
 * Warnings first. */
struct psk_find {
    uint8_t sev;        /* enum psk_sev: PSK_SEV_NOTE or PSK_SEV_WARN */
    uint8_t kind;       /* the sheet it opens: PSK_DIAG_FINDING, PSK_DIAG_DNS or PSK_DIAG_TCP */
    struct ppc_evt_net_event ev; /* PSK_DIAG_FINDING: the event */
    char text[72];      /* "TWO MACS CLAIM ONE ADDRESS" */
    char sub[96];       /* "192.0.2.77 · 00:42 · 3 TIMES" */
};
unsigned psk_findings(const struct psk_model *m, struct psk_find *out, unsigned max);

/* A device's two lines, "192.0.2.1 · ROUTER · DHCP SERVER" and
 * "02:00:5e:10:00:fe · 1 204 PKT · 1.2 MB · 38%"; its share of the
 * session's bytes, in tenths of a percent; and the host a list of its
 * packets filters on: its IPv4 address, "" when none is known (a MAC does
 * not filter IP packets, so such a device offers no list). */
void psk_net_host_lines(const struct psk_model *m, unsigned i, char *l1, size_t n1, char *l2, size_t n2);
unsigned psk_net_host_share(const struct psk_model *m, unsigned i);
const char *psk_net_host_filter(const struct psk_model *m, unsigned i);

/* The classes' shares of the session, largest first: "TCP 62% · UDP 20% ·
 * DNS 9%"; "NO PACKET YET" before the first. */
const char *psk_net_mix(const struct psk_model *m, char *buf, size_t buflen);
/* A lease time: "45 S", "30 MIN", "24 H", "7 D". */
const char *psk_fmt_lease(uint32_t seconds, char *buf, size_t buflen);

#endif

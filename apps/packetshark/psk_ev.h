/*
 * PacketShark's marks and their evidence in words (N1.0,
 * docs/apps/PACKETSHARK.md §4d): what the evidence page says of a mark the
 * helper answered for (core/pocketpcap/ppc_evid.h, the `evid` answer).
 *
 * The rule of every line here: an observation is a packet that was seen,
 * said as that and with the packet's number. What it may mean comes after,
 * as a sentence that says "may". A reply that was not seen is never called
 * missing: a request says whether its reply could pass this interface at
 * all, and a window says how much of it was watched. A protocol the capture
 * filter kept out, or whose analysis could not start, is said as that and
 * never as one that was quiet. There is no score, no percentage and no
 * verdict.
 *
 * Pure C over the model, no LVGL, no allocation (tests/psk_ev_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PSK_EV_H
#define PSK_EV_H

#include "psk_model.h"

#include <stddef.h>

#define PSK_EV_ROWS (2u * PPC_EV_KEEP)
/* The most lines the counts can have: a line for every kind. */
#define PSK_EV_COUNTS (PPC_EV_KINDS - 1u)

/* "Mark", or "Mark 2 of 3" when the capture holds more than one. */
const char *psk_ev_title(const struct psk_evidence *ev, char *buf, size_t buflen);
/* "04:13 INTO THE CAPTURE · 14:02:12 · ON HOLD": when it was set on the
 * capture's clock, on the wall clock when that is set, and what the capture
 * was doing when it was not simply capturing. */
const char *psk_ev_when(const struct psk_evidence *ev, char *buf, size_t buflen);

/* One window (0 before the mark, 1 after it), in three short lines. head:
 * its length, "60 S BEFORE", "19 S AFTER"; "NOTHING BEFORE" or "NOTHING
 * AFTER" for one that has none, "0 S AFTER" for one that has just begun.
 * why: what ended it short of a minute, or keeps it open - "THE CAPTURE
 * STARTED THEN", "STILL OPEN", "THE CAPTURE STOPPED THEN" - or "".
 * seen: "EVERY FRAME SEEN" ("EVERY CAPTURED FRAME SEEN" under a capture
 * filter), or what was not: "312 FRAMES LOST", "ON HOLD 14 S", "FRAMES LOST
 * AT THE MARK"; then the protocols it could not see: "ARP NOT CAPTURED",
 * "DNS NOT ANALYZED". Returns PSK_SEV_OK when every frame of the window was
 * seen and every protocol could be; PSK_SEV_WARN when frames were lost or
 * only counted, so something may be absent from it; PSK_SEV_NOTE when a
 * HOLD took part of it, the history holds less than it, a protocol could
 * not be seen, or it has no length. */
unsigned psk_ev_window(const struct psk_evidence *ev, unsigned side, char *head, size_t headlen, char *why,
                       size_t whylen, char *seen, size_t seenlen);

/* What was observed, counted: a line for a kind of message, with how many
 * were seen before the mark and after it ("DISCOVER", "4", "1"). A protocol
 * that was seen at all lists the messages of its ordinary exchange even at
 * zero - DISCOVER, OFFER, REQUEST and ACK; an ARP request and reply; a DNS
 * query and answer - so a count of none is on the page, not left to be
 * noticed missing. A protocol never seen has one line, "NONE SEEN". One the
 * capture could not see has one line, "NOT CAPTURED" (its filter kept it
 * out) or "NOT ANALYZED" (its analysis could not start), and "-" for its
 * figures. The figures are counts of what passed this interface and no
 * more. */
struct psk_ev_count {
    uint8_t cls;    /* PPC_CLASS_DHCP, PPC_CLASS_ARP or PPC_CLASS_DNS */
    uint8_t first;  /* the first line of its protocol */
    uint8_t none;   /* the protocol was not seen in either window */
    uint8_t blind;  /* the capture could not see the protocol at all */
    char name[28];
    char before[12];
    char after[12];
};
unsigned psk_ev_counts(const struct psk_evidence *ev, struct psk_ev_count *out, unsigned max);

/* The rows: the observations the helper kept, those before the mark first,
 * each side oldest first. when: "-12.4 S", "+3.1 S", from the mark. what:
 * the observation, under its protocol's name ("DISCOVER · 02:00:5e:00:00:21").
 * sub: what the packet carried, and whether its reply could be seen here.
 * alive: what the buffer still holds of the packet (enum ppc_ev_alive);
 * held: what the row says of that, "" while the packet is whole. */
struct psk_ev_row {
    uint8_t side;
    uint8_t cls;
    uint8_t alive;
    uint32_t seq;
    char when[16];
    char what[88];
    char sub[232];
    char held[72];
};
unsigned psk_ev_rows(const struct psk_evidence *ev, struct psk_ev_row *out, unsigned max);
/* A side's heading over its rows: "BEFORE THE MARK · 5 SEEN", "BEFORE THE
 * MARK · 12 OF 43 · NEAREST THE MARK" when not every observation is a row,
 * "AFTER THE MARK · NONE SEEN YET" while its window is filling, "AFTER THE
 * MARK · NONE SEEN". When the capture could not see a protocol, the empty
 * heading names the ones it could ("NO DHCP OR DNS SEEN"), or says "NOTHING
 * COULD BE OBSERVED". */
const char *psk_ev_side(const struct psk_evidence *ev, unsigned side, char *buf, size_t buflen);

/* What this evidence can and cannot tell, in plain sentences, a line each:
 * what one interface sees, the protocols this capture could not see and
 * why, and whatever limited these two windows (frames lost, some of them
 * too close to the mark to place, HOLD, a capture that started or stopped
 * inside them, a window still filling). */
const char *psk_ev_limits(const struct psk_evidence *ev, char *buf, size_t buflen);
/* What to say when neither window holds an observation: of the protocols
 * the capture could see, and a quiet segment only when every second of
 * both windows was watched. */
const char *psk_ev_nothing(const struct psk_evidence *ev, char *buf, size_t buflen);

/* How many marks the capture holds now: it keeps its newest eight. */
uint32_t psk_ev_held(const struct psk_model *m);

/* The classes a list of a mark's packets shows: DHCP, ARP and DNS. */
uint8_t psk_ev_classes(void);

#endif

/*
 * TROPO: an indication that packets are reaching this node over far more
 * relays than this mesh ordinarily has - the signature a tropospheric
 * opening leaves on a LoRa mesh, and a few other things too.
 *
 * Ported from RIFT on the T-Deck (vendor/RIFT, RiftLogic.h "tropospheric
 * opening detector", RiftRxLog.h riftTropoState, test_rift_logic.cpp
 * "Tropo"), with its three numbers kept:
 *
 *   - a packet is DEEP when its flood path has RIFT_TROPO_HOPS (20) relays
 *     or more;
 *   - an opening is DETECTED when RIFT_TROPO_NEEDED (5) deep packets have
 *     arrived within RIFT_TROPO_WINDOW_MS (15 min);
 *   - it stays ACTIVE until RIFT_TROPO_HOLD_MS (20 min) after the last deep
 *     packet, so one opening is one alert and not a flutter.
 *
 * and its one trap avoided: the path byte packs the hash size in bits 6-7 and
 * the hop count in bits 0-5, so "no path" (0xFF) reads as 63 hops. Here the
 * count arrives already split (mesh.rx path_hops, checked against the bytes
 * by rift_rxlog.c), and anything at RIFT_TROPO_IMPLAUSIBLE or above is still
 * not taken as a measurement.
 *
 * What is counted, and what is not (each is a rift_tropo_verdict):
 *
 *   - a reception MeshCore handled (verdict new or duplicate) - never a
 *     frame it rejected or lost track of;
 *   - of a flood or transport flood: a direct packet's path is the hops it
 *     has still to take, not the relays it came through;
 *   - with a hop path - never a TRACE, whose path bytes are SNRs;
 *   - not this node's own packet heard repeated back (`own`);
 *   - once per packet: MeshCore's packet hash is the packet, so a second
 *     copy of a deep packet - over another route, or the same one again -
 *     is a repeat and adds nothing. A frame with no hash cannot be told
 *     apart from its copies and is not counted;
 *   - received in the last window: a reception older than that (a late
 *     report, a clock that stepped) is not evidence of now.
 *
 * Unlike the original, the window slides: the five are any five distinct
 * deep packets no more than 15 minutes apart, not five that happened to
 * land in the same fixed quarter hour. Evidence that arrives while an
 * opening is active belongs to it and extends its hold; once its hold has
 * run out it is closed, and a deep packet after that is a candidate for a
 * new opening - one alone never is one.
 *
 * An opening's age is measured from its latest evidence, and its lifecycle
 * on the screen follows that age: DETECTED in its first hour (active, or
 * closed less than an hour after its last evidence), RECENT from one to
 * five hours, LAST after that. Nothing is detected before the first opening.
 *
 * Everything is on the shared monotonic clock (mesh.rx mono_ms, the time
 * given in), bounded (RIFT_TROPO_SEEN hashes, RIFT_TROPO_NEEDED candidates,
 * one opening), and decided from the evidence alone: whether an opening is
 * still active is a function of its last evidence and now, never of when a
 * timer happened to run. Nothing is persisted - the monotonic clock begins
 * again at boot - and nothing here is reset when the service goes away and
 * comes back: what was heard was heard.
 *
 * TROPO is an indication of unusual mesh propagation. It is not proof of
 * atmospheric conditions, of distance, or of any one path: a misconfigured
 * repeater loop or a bridged mesh can do the same, and the screen says so.
 *
 * No LVGL, no clock of its own: host-tested by tests/rift_tropo_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_TROPO_H
#define RIFT_TROPO_H

#include <stdint.h>

#define RIFT_TROPO_HOPS 20                               /* deep at or above */
#define RIFT_TROPO_NEEDED 5                              /* deep packets ... */
#define RIFT_TROPO_WINDOW_MS (15LL * 60 * 1000)          /* ... within this */
#define RIFT_TROPO_HOLD_MS (20LL * 60 * 1000)            /* active after the last */
/* 62 rather than 63, as in the original: one below the no-path sentinel is
 * not a plausible mesh depth either. */
#define RIFT_TROPO_IMPLAUSIBLE 62
/* The screen's lifecycle, by the age of an opening's latest evidence. */
#define RIFT_TROPO_DETECTED_MS (60LL * 60 * 1000)        /* red: the first hour */
#define RIFT_TROPO_RECENT_MS (5LL * 60 * 60 * 1000)      /* yellow: to five hours */
/* Distinct deep packets remembered, to tell a repeat from a new one: far
 * more than an opening's hold sees in practice, and 576 bytes. */
#define RIFT_TROPO_SEEN 64
#define RIFT_TROPO_HASH_LEN 8 /* MeshCore's packet hash, as mesh.rx gives it */

/* What one reception was, to the detector. */
enum rift_tropo_verdict {
    RIFT_TROPO_NOT_FRAME = 0, /* a note of RIFT's own, or nothing */
    RIFT_TROPO_REJECTED,      /* MeshCore never handled it */
    RIFT_TROPO_NO_PATH,       /* no hop path reported, or a TRACE's SNRs */
    RIFT_TROPO_NOT_FLOOD,     /* a direct packet: its path is hops to go */
    RIFT_TROPO_IMPLAUSIBLE_PATH,
    RIFT_TROPO_OWN,           /* this node's own packet, heard back */
    RIFT_TROPO_SHALLOW,       /* an ordinary depth: counted in the baseline */
    RIFT_TROPO_NO_HASH,       /* deep, but cannot be told from its copies */
    RIFT_TROPO_REPEAT,        /* deep, and already counted */
    RIFT_TROPO_STALE,         /* deep, but received too long ago */
    RIFT_TROPO_CANDIDATE,     /* deep and new: towards an opening */
    RIFT_TROPO_OPENED,        /* deep and new, and the one that made five */
    RIFT_TROPO_EXTENDED,      /* deep and new, during an active opening */
};

/* How the screen shows it (the lifecycle above). */
enum rift_tropo_level {
    RIFT_TROPO_NONE = 0,  /* no opening since RIFT's session began */
    RIFT_TROPO_DETECTED,  /* red */
    RIFT_TROPO_RECENT,    /* yellow */
    RIFT_TROPO_LAST,      /* grey */
};

struct rift_rx_entry; /* rift_rxlog.h */

struct rift_tropo_candidate {
    int64_t mono_ms;
    uint8_t hops;
};

struct rift_tropo {
    /* Deep packets already counted, by hash: a ring, newest last. */
    uint8_t seen[RIFT_TROPO_SEEN][RIFT_TROPO_HASH_LEN];
    int seen_next;
    int seen_count;
    /* Deep packets not part of an opening, oldest first. Never more than
     * NEEDED - 1 between calls: the NEEDED'th opens one. */
    struct rift_tropo_candidate cand[RIFT_TROPO_NEEDED];
    int cand_count;
    /* The opening: the current one, or the last one once it has closed. */
    int have_opening;
    int64_t first_ms;    /* its earliest evidence */
    int64_t detected_ms; /* the evidence that made it five */
    int64_t last_ms;     /* its latest evidence: its age and hold run from here */
    unsigned evidence;   /* distinct deep packets that belong to it */
    uint8_t peak_hops;
    unsigned openings;   /* since the session began */
    /* The ordinary depth of this mesh: the deepest usable flood path below
     * the threshold, over this window and the one before it (two fixed
     * quarter hours, so it is bounded and needs no list). */
    int64_t base_window_ms;
    uint8_t base_now;
    uint8_t base_before;
    /* Every deep reception, and how it was taken. */
    unsigned deep_seen;
    unsigned repeats;
    unsigned stale;
    unsigned revision; /* bumped by every change a screen would show */
};

/* What a screen shows at `now` (rift_tropo_read). */
struct rift_tropo_view {
    enum rift_tropo_level level;
    int active;          /* within the hold of its last evidence */
    int64_t age_ms;      /* now - its latest evidence */
    int64_t span_ms;     /* its first evidence to its latest */
    int64_t since_ms;    /* now - its first evidence */
    int64_t closes_in_ms;/* active: until the hold runs out */
    unsigned evidence;
    uint8_t peak_hops;
    unsigned openings;
    int candidates;      /* deep packets towards a new opening, in the window */
    int have_base;
    uint8_t base_hops;
};

void rift_tropo_init(struct rift_tropo *t);
/* One reception as RX LOG read it (rift_rxlog_apply calls this before it
 * files the entry), at now_mono on the shared monotonic clock. Returns what
 * it was taken as. A NULL tropo or entry is RIFT_TROPO_NOT_FRAME. */
enum rift_tropo_verdict rift_tropo_observe(struct rift_tropo *t, const struct rift_rx_entry *e,
                                           int64_t now_mono);
/* The same decision on bare fields, for the tests and for observe: hops is
 * the relay count, flood whether the route was a flood, handled whether
 * MeshCore handled it, hash NULL when the frame carried none. */
enum rift_tropo_verdict rift_tropo_step(struct rift_tropo *t, int64_t rx_mono, int hops, int flood,
                                        int handled, int own, const uint8_t *hash,
                                        int64_t now_mono);
/* Whether an opening is active at now: false once its hold has run out,
 * whether or not anything was received since. */
int rift_tropo_active(const struct rift_tropo *t, int64_t now_mono);
void rift_tropo_read(const struct rift_tropo *t, int64_t now_mono, struct rift_tropo_view *out);
/* The words the panel leads with: "TROPO DETECTED", "RECENT TROPO",
 * "LAST TROPO", "NO TROPO SEEN". */
const char *rift_tropo_level_word(enum rift_tropo_level level);

#endif

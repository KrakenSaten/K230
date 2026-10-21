/*
 * The explicit route of a direct message: the route builder's draft, the
 * route staged on a conversation, and what meshcored says it can do with
 * one (docs/api/mesh.md, mesh.path_preview and mesh.send "via";
 * docs/apps/rift/VISUAL_PATH_BUILDER.md).
 *
 * A route is the ordered list of repeaters a reader asked a message to be
 * forwarded through, held as their whole public keys - never a list index, a
 * hash or a name - so a node list that is refreshed, renamed or reordered
 * cannot point the draft at a different node. The sender (this device) and
 * the recipient (the conversation's peer) are implicit: neither is ever in
 * the list. An empty list is a route too: DIRECT with no relay, which is not
 * AUTO and is never a flood.
 *
 * What this is NOT is a statement about the radio. Nothing here, and
 * nothing a screen draws from it, says that two waypoints can hear each
 * other or that a message arrived: a preview checks identities and encoding
 * bounds against the service's node list, and only an ACK says a message got
 * there.
 *
 * Three pieces of state, each with one owner:
 *
 *   caps   what mesh.info said this run of meshcored offers. Absent is
 *          unsupported: an older service ignores unknown members, so `via`
 *          is never written to a service that did not say it takes it.
 *   draft  the builder's working copy, bound to one recipient. Every edit
 *          bumps gen; a preview answer is applied only when it answers the
 *          request in flight for the gen it was asked about.
 *   stage  the route a reader chose with USE ROUTE, bound to one
 *          conversation. The composer sends with it until the reader goes
 *          back to AUTO; a send never falls back to AUTO on its own.
 *
 * No LVGL, no I/O (tests/rift_route_test.c). The requests are written by
 * rift_ipc_path.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_ROUTE_H
#define RIFT_ROUTE_H

#include <cjson/cJSON.h>
#include <stddef.h>
#include <stdint.h>

struct rift_model;

/* The service's limit: 63 relays and 64 path bytes (docs/api/mesh.md,
 * mesh.path_preview). Wider hashes allow fewer; only the service knows the
 * width, so the byte limit is its to enforce and this is only the count. */
#define RIFT_ROUTE_MAX 63
/* The longest a key held here is (rift_model.h RIFT_KEY_HEX). */
#define RIFT_ROUTE_KEY 65
/* Words about a route: a refusal from the service, or why a tap did not
 * add a repeater. */
#define RIFT_ROUTE_WHY_MAX 120
/* The encoded path, as hex (64 bytes at most). */
#define RIFT_ROUTE_HEX_MAX 129

enum rift_route_check {
    RIFT_ROUTE_UNCHECKED = 0, /* not asked about yet, or edited since */
    RIFT_ROUTE_CHECKING,      /* a mesh.path_preview is out for this gen */
    RIFT_ROUTE_VALID,         /* the service took it, for this gen */
    RIFT_ROUTE_REFUSED,       /* the service refused it (error 2) */
    RIFT_ROUTE_UNANSWERED,    /* the service went before it answered */
};

/* What the service made of a route it took: its own numbers, never this
 * app's arithmetic. */
struct rift_route_encoding {
    int relays;
    int hash_bytes;
    int path_bytes;
    char path_hex[RIFT_ROUTE_HEX_MAX];
};

struct rift_route_draft {
    int active;                   /* the builder is open */
    char to[RIFT_ROUTE_KEY];      /* the recipient, fixed while it is open */
    char via[RIFT_ROUTE_MAX][RIFT_ROUTE_KEY];
    int count;
    int marked;                   /* the waypoint a reader picked in the list, or -1 */
    unsigned gen;                 /* bumped by every edit */

    enum rift_route_check check;
    unsigned checked_gen;         /* the gen check is about */
    int request;                  /* the preview in flight, 0 for none */
    struct rift_route_encoding enc;
    char why[RIFT_ROUTE_WHY_MAX]; /* the refusal, when REFUSED or UNANSWERED */
    /* The last thing a tap could not do (a node that is not a repeater, one
     * already in the route): said in the panel, cleared by the next edit. */
    char note[RIFT_ROUTE_WHY_MAX];
};

struct rift_route_stage {
    int active;
    char conv[RIFT_ROUTE_KEY];
    char via[RIFT_ROUTE_MAX][RIFT_ROUTE_KEY];
    int count;
    struct rift_route_encoding enc; /* as the preview that let it be staged said */
};

struct rift_route {
    /* mesh.info capabilities, for the connection that read them. */
    int caps_known;
    int can_preview;
    int can_explicit;

    struct rift_route_draft draft;
    struct rift_route_stage stage;

    /* A SEND on the staged route is checked before mesh.send is written:
     * the route it is checking, copied so a reader editing the stage in the
     * meantime cannot change what this submission sends, and the preview
     * request that answers it. */
    char send_via[RIFT_ROUTE_MAX][RIFT_ROUTE_KEY];
    int send_count;
    int send_check_request;
};

/* ---- the service ------------------------------------------------------------ */

/* A mesh.info result: the two capabilities are taken only when they are the
 * JSON value true. Anything else - absent, false, a string - is unsupported. */
void rift_route_apply_info(struct rift_route *r, const cJSON *result);
/* The connection went: what the next service offers is not known until it
 * says, and a preview that was out has no answer coming. */
void rift_route_service_lost(struct rift_route *r);
/* Whether this service can preview and send an explicit route. */
int rift_route_supported(const struct rift_route *r);

/* ---- the draft ---------------------------------------------------------------- */

/* Open the builder for the conversation with `to`. When a route is staged on
 * that conversation the draft starts from it; otherwise empty. Fails (-1)
 * when `to` is not a whole public key. */
int rift_route_open(struct rift_route *r, const char *to);
/* Close the builder without staging anything (CANCEL). The stage, and the
 * contact's learned route, are untouched. */
void rift_route_close(struct rift_route *r);

/* Whether key may be appended to the draft. 0 when it may; -1 with the
 * reason in why otherwise: not in the node list, not a repeater, this
 * device, the recipient, already in the route, or the route is full. */
int rift_route_eligible(const struct rift_route *r, const struct rift_model *m, const char *key,
                        char *why, size_t why_len);
/* Append a repeater (by whole key). 0, or -1 with the reason in draft.note. */
int rift_route_add(struct rift_route *r, const struct rift_model *m, const char *key);
/* Take the last repeater off. -1 when there was none. */
int rift_route_undo(struct rift_route *r);
/* Remove every repeater. */
void rift_route_clear(struct rift_route *r);
/* Remove the waypoint at index and every one after it. -1 when out of range. */
int rift_route_cut(struct rift_route *r, int index);
/* Pick a waypoint in the list (-1 for none). */
void rift_route_mark(struct rift_route *r, int index);
/* The index of key in the draft, or -1. */
int rift_route_index(const struct rift_route *r, const char *key);

/* How many waypoints of the draft (recipient included) the node list no
 * longer holds. A draft with one is not sent: it is reviewed, never remapped. */
int rift_route_missing(const struct rift_route *r, const struct rift_model *m);

/* Whether the draft needs a preview now: open, supported, nothing in flight,
 * and not yet checked at this gen. */
int rift_route_wants_check(const struct rift_route *r);
/* A preview was written for the draft as it stands, under this request id. */
void rift_route_check_sent(struct rift_route *r, int request);
/* The answer to a preview. Applied only when request is the one in flight
 * for the draft; anything else is stale and changes nothing (returns -1).
 * A result that is not the service's "valid explicit route, nothing
 * transmitted" for this many relays is taken as a refusal. */
int rift_route_check_answered(struct rift_route *r, int request, const cJSON *result);
int rift_route_check_refused(struct rift_route *r, int request, const char *why);
/* Whether USE ROUTE may stage the draft: checked valid at this gen, still
 * supported, and every waypoint still known. */
int rift_route_usable(const struct rift_route *r, const struct rift_model *m);
/* Stage the draft on its conversation and close the builder. -1 when it is
 * not usable. */
int rift_route_use(struct rift_route *r, const struct rift_model *m);

/* ---- the stage -------------------------------------------------------------------- */

/* The route staged on conv, or NULL: then the conversation is AUTO. */
const struct rift_route_stage *rift_route_staged(const struct rift_route *r, const char *conv);
/* Back to AUTO for conv. */
void rift_route_auto(struct rift_route *r, const char *conv);

/* Parse a preview result into enc. 0 when it is the service's word that the
 * route is valid, explicit and was not transmitted, for `relays` relays. */
int rift_route_parse_preview(const cJSON *result, int relays, struct rift_route_encoding *enc);

/* ---- words --------------------------------------------------------------------------- */

/* "YOU > RPT-NORD > RPT-7 > HYTTA": the route in order, named from the node
 * cache, a waypoint the cache no longer holds by its key's first bytes. */
void rift_route_chain(const struct rift_model *m, const char *to, const char (*via)[RIFT_ROUTE_KEY],
                      int count, char *out, size_t out_len);
/* "2 RELAYS", "1 RELAY", "NO RELAY". */
const char *rift_route_relays_word(int count, char *buf, size_t len);

#endif

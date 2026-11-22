/*
 * gatewayd: sending (ADR-019 rule 27 and "Limits"; docs/api/gateway.md §8;
 * THREAT_MODEL T7 and T17).
 *
 * What a client asks to have sent is checked here, and what the gateway
 * remembers about it is kept here: for each paired client the last
 * GW_SEND_RECORDS client_msg_id values with what became of them, and the
 * times of its last sends for the rate limit.
 *
 * One client_msg_id is transmitted at most once. A record is in one of
 * these states:
 *
 *   PENDING    meshcored was asked and has not answered
 *   DONE       meshcored accepted it: a repeat is answered from the record
 *   UNCERTAIN  no answer came (a deadline, a lost connection): whether
 *              meshcored took it is not known. A repeat first asks
 *              meshcored for its messages (RESOLVING) and transmits only
 *              when that list proves the message is not there
 *   UNKNOWN    it can no longer be found out (meshcored started again, the
 *              gateway's connection to it was made anew since, or its list
 *              proves nothing): never transmitted again under this id; a
 *              person decides
 *
 * A request meshcored refused (an error of its own, codes 1 to 6) was not
 * transmitted and leaves no record: the same id may be tried again.
 *
 * Pure: cJSON only, no I/O; the clock is passed in.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef GATEWAYD_GW_SEND_H
#define GATEWAYD_GW_SEND_H

#include "gw.h"
#include "gw_journal.h"

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stdint.h>

#define GW_SEND_RECORDS 64        /* client_msg_id values remembered per client */
#define GW_SEND_PER_MIN 20        /* ADR-019 "Limits" */
#define GW_SEND_WINDOW_MS 60000
#define GW_SEND_INFLIGHT 8        /* sends of one client waiting for meshcored */
#define GW_CMID_MIN 16
#define GW_CMID_MAX 36
#define GW_SEND_TEXT_MAX 160      /* bytes: the upstream limit of a direct message */
#define GW_SEND_VIA_MAX 63
#define GW_SEND_CHANNEL_MAX 255   /* a slot number; meshcored says which exist */
#define GW_MSG_ID_MAX 9007199254740991.0
/* meshcored keeps at least this many messages of a kind (it keeps 64): fewer
 * than this after a send cannot have pushed the send out of its list. */
#define GW_SEND_RING_MIN 16

/* A request, checked. */
struct gw_send_req {
    bool resend;
    char cmid[GW_CMID_MAX + 1];
    char to[GW_FP_HEX + 1];             /* lower case; "" for a channel */
    int channel;                        /* -1 for a node */
    char text[GW_SEND_TEXT_MAX + 1];
    bool has_via;
    double message_id;                  /* resend */
    char run_id[GW_RUN_ID_MAX + 1];     /* resend */
    uint64_t digest;                    /* of everything above but cmid */
};

enum gw_send_state {
    GW_SS_FREE = 0,
    GW_SS_PENDING,
    GW_SS_RESOLVING,
    GW_SS_UNCERTAIN,
    GW_SS_DONE,
    GW_SS_UNKNOWN,
};

struct gw_send_rec {
    enum gw_send_state state;
    char cmid[GW_CMID_MAX + 1];
    uint64_t digest;
    bool resend;
    char to[GW_FP_HEX + 1];
    int channel;
    char text[GW_SEND_TEXT_MAX + 1];
    double message_id;      /* resend: the message; else, once DONE: what it became */
    double id_before;       /* the highest message id seen when meshcored was asked */
    double attempts_before; /* resend: the message's attempts then; -1: not known */
    char run_id[GW_RUN_ID_MAX + 1];
    uint64_t order;         /* age: the oldest record that is not waiting is replaced */
    cJSON *result;          /* DONE: the answer, without `duplicate` */
    /* For the caller (main.c), untouched here but for being cleared: */
    uint64_t tag;           /* the request to meshcored that is waited for */
    uint64_t conn;          /* which connection to meshcored the message was passed on */
    uint64_t sid;           /* the session that waits for the answer, 0: none */
    double rid;             /* and its request id */
    bool dup;               /* that session's answer carries duplicate: true */
    cJSON *up;              /* RESOLVING: what to send if the list says it is not there */
};

struct gw_send_client {
    bool used;
    uint8_t fp[GW_FP_LEN];
    uint64_t next_order;
    int win_n;
    int64_t win[GW_SEND_PER_MIN];   /* when its last sends were passed on */
    struct gw_send_rec r[GW_SEND_RECORDS];
};

struct gw_send {
    struct gw_send_client c[GW_MAX_CLIENTS];
};

void gw_send_init(struct gw_send *gs);
void gw_send_free(struct gw_send *gs);

/* mesh.send {to | channel, text, client_msg_id, via[]} or mesh.resend
 * {message_id, run_id, client_msg_id}: any other member, a missing one or
 * one of the wrong shape is -1. */
int gw_send_parse(const cJSON *params, bool resend, struct gw_send_req *q);
/* What meshcored is sent for it, written from nothing; `params` is the
 * object q was parsed from (for via). NULL when there is no memory. */
cJSON *gw_send_upstream(const struct gw_send_req *q, const cJSON *params);

/* The client's records; made when it has none yet. `paired` says whether a
 * fingerprint is still a paired client: the records of one that is not are
 * given up when room is needed. NULL when there is no room. */
struct gw_send_client *gw_send_client(struct gw_send *gs, const uint8_t fp[GW_FP_LEN],
                                      bool (*paired)(const uint8_t fp[GW_FP_LEN]));
struct gw_send_rec *gw_send_by_tag(struct gw_send *gs, uint64_t tag, struct gw_send_client **c);
int gw_send_inflight(const struct gw_send_client *c);

enum gw_send_action {
    GW_SA_FORWARD,      /* a new record, PENDING: pass it to meshcored now */
    GW_SA_LOOKUP,       /* an UNCERTAIN record, now RESOLVING: ask for the messages */
    GW_SA_REPLY,        /* DONE: answer from rec->result with duplicate: true */
    GW_SA_IN_PROGRESS,  /* PENDING or RESOLVING: an answer is on its way */
    GW_SA_MISMATCH,     /* the id was used for another message (code 2) */
    GW_SA_UNKNOWN,      /* the outcome cannot be found out (code 3) */
    GW_SA_STALE_RUN,    /* resend of a message of an earlier run (code 3) */
    GW_SA_RATE,         /* code 10 */
    GW_SA_BUSY,         /* code 5 */
    GW_SA_UNAVAILABLE,  /* meshcored is not there (code 9); nothing changed */
};

/* Decides what happens to a request. run_id is meshcored's run now, or NULL
 * when it is not connected; id_before the highest message id of that run
 * seen so far. *rec is the record for every action but MISMATCH, STALE_RUN,
 * RATE, BUSY and UNAVAILABLE-without-a-record. */
enum gw_send_action gw_send_begin(struct gw_send_client *c, const struct gw_send_req *q, const char *run_id,
                                  double id_before, int64_t now_ms, struct gw_send_rec **rec);

enum gw_send_outcome {
    GW_SO_DONE,       /* accepted: rec->result holds the answer */
    GW_SO_REFUSED,    /* meshcored refused it, nothing was sent: the record is gone */
    GW_SO_UNCERTAIN,  /* no answer */
    GW_SO_UNKNOWN,    /* an answer that says accepted without saying what */
};
/* meshcored's answer to a PENDING record: its result, or NULL and a code. */
enum gw_send_outcome gw_send_answered(struct gw_send_rec *rec, const cJSON *result, int code);

enum gw_send_look {
    GW_SL_FOUND,    /* meshcored has it: the record is DONE */
    GW_SL_ABSENT,   /* proven not there: it may be sent (the record stays RESOLVING) */
    GW_SL_UNKNOWN,  /* the list cannot say: the record is UNKNOWN */
};
/* meshcored's message list (the `messages` array) against a RESOLVING
 * record. */
enum gw_send_look gw_send_looked(struct gw_send *gs, struct gw_send_rec *rec, const cJSON *messages);

/* Back to UNCERTAIN: the list did not come, or the send could not go out. */
void gw_send_unresolved(struct gw_send_rec *rec);
/* Gives a record up (a request that never left). */
void gw_send_release(struct gw_send_rec *rec);

/* The answer to a client: a copy of rec->result with `duplicate`. */
cJSON *gw_send_result(const struct gw_send_rec *rec, bool duplicate);

#endif

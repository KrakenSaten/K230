/*
 * gatewayd: Vision, as a paired client may see and steer it (ADR-019
 * amendment 1 and ADR-006 amendment 1, accepted 2026-11-23;
 * docs/design/companion/VISION_PREVIEW.md sections 5 to 7;
 * docs/api/gateway.md section 9a).
 *
 * The gateway asks the Vision app, through the shell's socket, for six
 * things and nothing else: vision.status, vision.share, vision.mode,
 * vision.set, vision.reset and, for itself alone, vision.pull. It does not
 * listen to the shell: what a client is told of Vision is made here from
 * the answers to the gateway's own vision.status, asked once a second while
 * a client holds the topic, and a picture is the answer to the gateway's own
 * vision.pull, asked only when the one viewer has acknowledged the last.
 *
 * This file is everything of that which needs no socket:
 *
 *   - the filters. An answer of the unit is never passed on: it is read by
 *     name, checked, and written again from nothing. A word is a word of the
 *     list, a number is whole and in range, a picture is base64 of bounded
 *     length that begins as a JPEG does, a box is nine whole numbers.
 *     Nothing else of an answer reaches a client, whatever a later Vision
 *     adds to it. The modes FACE, RECOGNIZE and READ can be named by the
 *     unit (a client is told that the unit is in one) and never by a client.
 *   - the parameters. What a client sent is checked and written again.
 *   - what clients were last told, so that vision.state is said once per
 *     change.
 *   - the one viewer: whose the preview is, which picture it was sent, and
 *     whether it has acknowledged it; and how often a client may ask.
 *
 * Nothing here holds a picture: the one the gateway holds lives from the
 * unit's answer to the moment it is queued for the viewer, in main.c.
 *
 * Pure: cJSON only, no I/O, the clock passed in.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef GATEWAYD_GW_VISION_H
#define GATEWAYD_GW_VISION_H

#include "gw.h"

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stdint.h>

#define GW_VISION_POLL_MS 1000      /* vision.status, while a client holds the topic or views */
#define GW_VISION_CALL_MS 2000      /* any request to the shell: answered by then, or given up */
#define GW_VISION_INTERVAL_MS 200   /* between two pictures: 5 a second at most */
#define GW_VISION_RETRY_MS 40       /* after an answer that had no new picture */
#define GW_VISION_ACK_MS 5000       /* a sent picture not acknowledged for this long ends the preview */
#define GW_VISION_PER_MIN 20        /* vision.share {on: true}, vision.mode, vision.set, vision.reset together, per client */

#define GW_VISION_JPEG_MAX 65536    /* bytes of JPEG, decoded */
#define GW_VISION_B64_MAX 87384     /* and as base64 with its padding: ceil(65536 / 3) * 4 */
#define GW_VISION_ANSWER_MAX 90112  /* the unit's whole vision.pull answer, serialised (the shell's bound) */
#define GW_VISION_BOXES_MAX 24
#define GW_VISION_BOX_FIELDS 9      /* id, cls, conf, x, y, w, h, dir, kmh10 */
#define GW_VISION_BOX_ABS_MAX 1000000.0
#define GW_VISION_SIDE_MAX 640
#define GW_VISION_AGE_MAX 3600000.0
#define GW_VISION_SEQ_MAX 999999999999999.0
#define GW_VISION_FIGURE_MAX 999999999.0
#define GW_VISION_MODES_MAX 16
#define GW_VISION_VALUE_MAX 16      /* characters of a setting's value */
#define GW_VISION_MODEL_MAX 48
#define GW_VISION_LABELS_MAX 400
#define GW_VISION_LABEL_MAX 24
#define GW_VISION_CLIENT 17         /* who views, to the unit: 16 hex digits of the fingerprint */
#define GW_VISION_SIG 512

/* ---- filters and parameters (rows of gw_policy.c) --------------------------- */

/* How a status reads to the client that asks: nobody views, it is the
 * viewer itself, or another client is. */
enum gw_vision_view { GW_VV_NONE = 0, GW_VV_SELF, GW_VV_OTHER };

/* vision.status for a client: {"available": false}, or the contract's
 * fields and no other. `share` is live for the viewer alone, busy for every
 * other client while somebody views, else off or stopped; who views is
 * never passed on. NULL when the answer is not an object or memory is
 * short. */
cJSON *gw_vision_status(const cJSON *result, enum gw_vision_view view);
/* The same for a client that is not the viewer while nobody is (the row's
 * own filter; main.c answers through gw_vision_status with the view). */
cJSON *gw_vision_status_row(const cJSON *result);
/* vision.share: {state}, a word of off, live, stopped; NULL when the unit
 * did not say one. */
cJSON *gw_vision_shared(const cJSON *result);
/* vision.mode, vision.set, vision.reset: {} whatever the unit said. */
cJSON *gw_vision_done(const cJSON *result);

/* vision.share {on}: a boolean and nothing else. Who views and what it is
 * called are added by the gateway. */
int gw_vision_params_share(const cJSON *in, cJSON **out);
/* vision.ack {seq}: a whole number and nothing else. */
int gw_vision_params_ack(const cJSON *in, cJSON **out);
/* vision.mode {mode}: one of detect track traffic segment instances color
 * edge trace. Never face, recognize or read. */
int gw_vision_params_mode(const cJSON *in, cJSON **out);
/* vision.set {key, value}: one of the eight settings with one of its own
 * values. */
int gw_vision_params_set(const cJSON *in, cJSON **out);

/* One answer to the gateway's own vision.pull {after_seq}. */
enum gw_vision_pulled {
    GW_VF_NONE = 0, /* {seq} alone: no picture newer than after_seq yet */
    GW_VF_PICTURE,  /* *data is the vision.frame event's data (the caller's), *seq its number */
    GW_VF_BAD,      /* something of it is not as the contract says: nothing of it is used */
};
enum gw_vision_pulled gw_vision_frame(const cJSON *result, double after_seq, cJSON **data, double *seq);

/* ---- what clients were told, and the one viewer ----------------------------- */

struct gw_vision {
    /* what vision.state was last raised for */
    bool told;
    char sig[GW_VISION_SIG];
    /* the one viewer */
    bool viewing;
    uint64_t sid;           /* its session */
    uint8_t fp[GW_FP_LEN];  /* and its client */
    uint64_t began;         /* the place, among the gateway's requests to the unit, of the one that made it */
    double last_seq;        /* the newest picture the unit has handed over in this preview */
    bool sent;              /* one was sent to the viewer */
    bool waiting;           /* and is not acknowledged yet */
    double sent_seq;
    int64_t sent_ms;
    int64_t next_ms;        /* no vision.pull before this */
    unsigned long bad;      /* answers to vision.pull that were dropped whole */
    /* when each client last asked for something that reaches the unit */
    struct {
        bool used;
        uint8_t fp[GW_FP_LEN];
        int n, at;
        int64_t ms[GW_VISION_PER_MIN];
    } rate[GW_MAX_CLIENTS];
};

/* What one answer to the gateway's own vision.status said, for main.c to
 * act on. Nothing of it is passed on. */
struct gw_vision_seen {
    bool available;
    bool live;                      /* the unit says it shares */
    bool biometric;                 /* its mode is face, recognize or read */
    char client[GW_VISION_CLIENT];  /* with whom, by its own word; empty when it did not say */
};

void gw_vision_init(struct gw_vision *v);

/* One answer to the gateway's own vision.status, as the unit gave it.
 *   *changed  availability, owner, mode, modes, settings or the unit's own
 *             share word are not what vision.state was last raised for (or
 *             none was raised yet): raise it now
 *   *stats    the vision.stats event's data (the caller's), or NULL when
 *             Vision is not in front
 *   *seen     what main.c reconciles the viewer with */
void gw_vision_step(struct gw_vision *v, const cJSON *raw, bool *changed, cJSON **stats, struct gw_vision_seen *seen);
/* The shell is gone: true when clients had been told of a Vision that was
 * there, so that vision.state is owed. */
bool gw_vision_lost(struct gw_vision *v);

/* This session's client is the viewer from now; `order` is the place of the
 * vision.share that the unit answered "live". */
void gw_vision_viewer(struct gw_vision *v, const uint8_t *fp, uint64_t sid, uint64_t order, int64_t now_ms);
void gw_vision_viewer_end(struct gw_vision *v);
enum gw_vision_view gw_vision_view_of(const struct gw_vision *v, uint64_t sid);

/* May the next picture be asked for now: there is a viewer, the interval
 * since the last picture (or the retry time since an empty answer) has
 * passed, and the viewer has acknowledged what it was sent. */
bool gw_vision_pull_due(const struct gw_vision *v, int64_t now_ms);
/* The answer had no new picture. */
void gw_vision_none(struct gw_vision *v, int64_t now_ms);
/* Picture `seq` was queued for the viewer. */
void gw_vision_sent(struct gw_vision *v, double seq, int64_t now_ms);
/* Picture `seq` was not sent (the viewer is behind on reading, or the
 * answer was dropped: then seq is 0): no acknowledgement is owed for it. */
void gw_vision_skipped(struct gw_vision *v, double seq, int64_t now_ms);
/* vision.ack {seq} of this session: true when it is the viewer's and for
 * the picture last sent. Anything else changes nothing. */
bool gw_vision_ack(struct gw_vision *v, uint64_t sid, double seq);
/* The viewer has not acknowledged a sent picture for GW_VISION_ACK_MS. */
bool gw_vision_ack_late(const struct gw_vision *v, int64_t now_ms);

/* One more request of this client that reaches the unit: false when it has
 * made GW_VISION_PER_MIN in the last minute (and this one is not counted). */
bool gw_vision_rate(struct gw_vision *v, const uint8_t *fp, int64_t now_ms);

#endif

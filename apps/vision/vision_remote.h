/*
 * Vision as a paired companion is shown it: what the Vision app answers on
 * the shell's socket to the gateway's `vision.*` requests (ADR-006 amendment
 * 1 and ADR-019 amendment 1, accepted 2026-11-23;
 * docs/design/companion/VISION_PREVIEW.md section 5.2).
 *
 * This file is the part with no screen in it: the answers written from the
 * model and the session, the words a companion may send and what they
 * change, and the one rule that is checked everywhere - a picture of FACE,
 * RECOGNIZE or READ never leaves the unit, and none of the three can be
 * chosen from outside it. vision_app.c holds who is being shown the picture,
 * the mark and STOP.
 *
 * Every answer is made from what the app already holds: no file, no wait.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VISION_REMOTE_H
#define VISION_REMOTE_H

#include "vision_model.h"
#include "vision_session.h"

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stdint.h>

/* The sharing of this Vision screen's picture. */
enum vision_share_state {
    VISION_SHARE_OFF = 0, /* nobody is shown it */
    VISION_SHARE_LIVE,    /* one companion is */
    VISION_SHARE_STOPPED  /* STOP was tapped here: nobody, until it is allowed here again or Vision is opened anew */
};

/* The viewer's name on the mark: at most this many bytes, printable. */
#define VISION_REMOTE_NAME_MAX 33
/* The viewer's key, as the gateway names it: 16 hex digits. */
#define VISION_REMOTE_CLIENT_MAX 17
/* A viewer that has not asked for a picture for this long is gone (its
 * gateway died, or lost it without saying so): the sharing ends here. */
#define VISION_REMOTE_IDLE_MS 8000
/* The class names a status carries: left out altogether when they would
 * not fit the answer (a detector with hundreds of long names). */
#define VISION_REMOTE_LABELS_MAX 400
#define VISION_REMOTE_LABEL_BYTES 5000

/* FACE, RECOGNIZE, READ: never shared, never chosen from outside. */
bool vision_remote_private(enum vision_mode mode);
/* Whether the picture can be shared right now: streaming, a helper that
 * can make it, a mode that may be shown. */
bool vision_remote_shareable(const struct vision_model *m, const struct vision_session *s);

/* vision.status. `client` is the viewer's key while live, else ignored.
 * NULL only when memory is short. */
cJSON *vision_remote_status(const struct vision_model *m, const struct vision_session *s,
                            enum vision_share_state share, const char *client);

/* The picture of a vision.pull: {seq, w, h, age_ms, jpeg, boxes[]} from the
 * session's newest shared picture, numbered seq by the caller. NULL when
 * there is none or memory is short. */
cJSON *vision_remote_picture(const struct vision_session *s, uint32_t seq, int64_t now_ms);

/* vision.mode {mode}: 0 and the model's actions in *acts; or a pocketipc
 * error code with *message. A private mode, or one this unit cannot run, is
 * refused. */
int vision_remote_mode(struct vision_model *m, const cJSON *params, unsigned *acts, const char **message);
/* vision.set {key, value}: one of the settings Vision's own sheets offer,
 * for the mode in force. */
int vision_remote_set(struct vision_model *m, const cJSON *params, unsigned *acts, const char **message);

/* vision.share {on, client, name}: read and checked. 0, or an error code
 * with *message. client and name are copied, cleaned, into the buffers. */
int vision_remote_share_params(const cJSON *params, bool *on, char client[VISION_REMOTE_CLIENT_MAX],
                               char name[VISION_REMOTE_NAME_MAX], const char **message);
/* vision.pull {after_seq}: 0 when absent. */
uint32_t vision_remote_after(const cJSON *params);

/* Base64 of n bytes into out (NUL-terminated); out must hold
 * 4 * ((n + 2) / 3) + 1. Returns the characters written. */
size_t vision_remote_base64(const uint8_t *in, size_t n, char *out);

#endif

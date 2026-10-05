/*
 * One conversation, drawn: the header that says how this peer is reached,
 * the messages in the order they happened, and the composer.
 *
 * It is a component of COMMS the way rift_detail is a component of NODES -
 * the same object in the portrait column and in the landscape middle pane,
 * built once and told what to show.
 *
 * Two things it will not do, both of them the point:
 *
 *   - It never marks a message sent. The state under every message is the
 *     service's word, and "sent" and "delivered" stay different answers
 *     (docs/api/mesh.md: accepted is not transmitted).
 *   - It prints no route for an individual message. The API carries a path
 *     on a node and not on a message, so the route lives in the header,
 *     where it is the peer's current path and is true, rather than under
 *     each line where it would be a guess about the frame that carried it.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RIFT_THREAD_H
#define RIFT_THREAD_H

#include "rift_app.h"

/* Message rows built at once. The thread is read from its end, and what
 * does not fit is counted and said ("14 EARLIER") rather than dropped
 * silently. A message arriving appends one row and, once the window is
 * full, drops the oldest: the rest are not rebuilt (rift_thread.c). */
#define RIFT_THREAD_ROWS 64

struct rift_thread;

struct rift_thread *rift_thread_create(struct rift_app *app, lv_obj_t *parent);
void rift_thread_destroy(struct rift_thread *t);
lv_obj_t *rift_thread_root(struct rift_thread *t);

/* Draw the conversation with this peer. peer may be NULL, which is the
 * "nothing is open" state and is drawn as such rather than left blank.
 * conv is that peer's summary, or NULL when it has no messages yet. */
void rift_thread_refresh(struct rift_thread *t, const char *peer, const struct rift_conv *conv);

/* Portrait shows the composer here, under the messages, with a 56 px field
 * and SEND (handoff §9). Landscape does not: there the command line is the
 * composer, and two bars would take an eighth of the height that mode has. */
void rift_thread_shape(struct rift_thread *t, int wide);

/* What is in the portrait composer, and how to empty it once a request has
 * actually been written. */
const char *rift_thread_composer_text(const struct rift_thread *t);
lv_obj_t *rift_thread_field(struct rift_thread *t);
void rift_thread_composer_clear(struct rift_thread *t);

/* Why the composer cannot be used, or NULL when it can. Shown rather than
 * the button merely being dead (DS §9). Lives here because the landscape
 * command line asks it the same question. */
const char *rift_thread_refusal(const struct rift_app *app);

/* The note under the thread (rift_thread_note.c): what became of the last
 * send, what the composer cannot do, or what an empty thread means. */
void rift_thread_note_paint(struct rift_app *app, lv_obj_t *note, const char *peer, int shown);

/* The actions on one message (ui/rift_msgact.h). A key while they are open
 * is theirs (1); select_newest opens them on the newest message by key,
 * returning 1 when there was one to open them on. */
struct rift_msgact;
int rift_thread_key(struct rift_thread *t, uint32_t key);
int rift_thread_select_newest(struct rift_thread *t);
struct rift_msgact *rift_thread_actions(struct rift_thread *t);

#endif

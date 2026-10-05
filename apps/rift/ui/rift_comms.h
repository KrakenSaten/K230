/*
 * COMMS: the conversations, the open thread and the composer.
 *
 * What this draws is what meshcored can answer for, and no more: direct
 * messages between two nodes and the group channels the service holds
 * (mesh.messages, mesh.channels, mesh.send, mesh.message, mesh.channel),
 * the channels in the same list with a "#" glyph as the approved design
 * merges them. Landscape is a console (DS §37.2): a narrow list, the
 * thread with the rest of the width, a one-line header carrying the route,
 * and a details pane only while a reader has asked for it.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RIFT_COMMS_H
#define RIFT_COMMS_H

#include "rift_app.h"

lv_obj_t *rift_comms_create(struct rift_app *app, lv_obj_t *parent);
void rift_comms_destroy(struct rift_app *app);
void rift_comms_refresh(struct rift_app *app);
/* Lay the panes out for the room there is (portrait column, landscape
 * split), as the other sections do. */
void rift_comms_shape(struct rift_app *app);
/* Arrows, Enter and Esc while COMMS is showing. Returns 1 when the key was
 * used. */
int rift_comms_key(struct rift_app *app, uint32_t key);
/* Open the conversation before (dir < 0) or after (dir > 0) the open one in
 * the list's order, stopping at either end. Returns 1 when there was a list
 * to step through. */
int rift_comms_step(struct rift_app *app, int dir);
/* The portrait composer's field (the thread's), or NULL before it is built. */
lv_obj_t *rift_comms_field(const struct rift_app *app);

/* Send text to the open conversation. Called by the portrait composer's
 * SEND and by the landscape command line's Enter, which are the only two
 * ways anything in RIFT transmits. Empty or unsendable text is refused
 * with a reason on screen and nothing is written. */
void rift_comms_submit(struct rift_app *app, const char *text);
/* RESEND of the message with this id (rift_ipc_resend_message), from its
 * action in the thread (ui/rift_msgact.c) and nowhere else. */
void rift_comms_resend(struct rift_app *app, int64_t message_id);
/* The actions on the open thread's newest message, opened by key (LEFT on an
 * empty composer). Returns 1 when there was a message to open them on. */
int rift_comms_select_message(struct rift_app *app);
/* The open thread's message actions (ui/rift_msgact.h), for the tests. */
struct rift_msgact;
struct rift_msgact *rift_comms_actions(const struct rift_app *app);

/* The peer whose thread is open, or NULL. */
const char *rift_comms_open_peer(const struct rift_app *app);
/* Conversation rows in the pool (rift_conv_list.h), for the test: bounded
 * by the pane, not by the count. */
int rift_comms_rows_built(const struct rift_app *app);
/* What the landscape command line should say it is addressing, for its
 * placeholder and its right-hand hint. Writes "" when nothing is open. */
void rift_comms_target_label(const struct rift_app *app, char *out, size_t out_len);

#endif

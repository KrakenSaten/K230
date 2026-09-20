/*
 * COMMS: the conversations, the open thread and the composer.
 *
 * What this draws is what meshcored can answer for, and no more. The API is
 * direct messages between two nodes (mesh.messages, mesh.send,
 * mesh.message); it has no group channels at all - MAX_GROUP_CHANNELS is
 * left undefined in protocols/meshcore, so upstream's channel code is not
 * compiled, meshcored's onChannelMessageRecv is an empty override and
 * docs/api/mesh.md lists channels under "Not in v0". The approved design
 * merges channels into this list with a "#" glyph; this build says that
 * they are not in the service rather than drawing an empty channel or a
 * plausible one, because a channel row nobody could send to would be this
 * app inventing a feature the mesh does not have.
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

/* Send text to the open conversation. Called by the portrait composer's
 * SEND and by the landscape command line's Enter, which are the only two
 * ways anything in RIFT transmits. Empty or unsendable text is refused
 * with a reason on screen and nothing is written. */
void rift_comms_submit(struct rift_app *app, const char *text);

/* The peer whose thread is open, or NULL. */
const char *rift_comms_open_peer(const struct rift_app *app);
/* What the landscape command line should say it is addressing, for its
 * placeholder and its right-hand hint. Writes "" when nothing is open. */
void rift_comms_target_label(const struct rift_app *app, char *out, size_t out_len);

#endif

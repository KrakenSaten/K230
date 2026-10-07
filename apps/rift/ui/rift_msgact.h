/*
 * What can be done with one message in the open thread: a compact bar under
 * the messages, opened by holding a message (touch) or by LEFT on an empty
 * composer (keys), with only the actions that message has:
 *
 *   REPLY   an incoming channel message that names its sender: the reply
 *           prefix (rift_reply.h) goes into the composer, to be read and
 *           finished there. Nothing is sent by it.
 *   COPY    the message's words go into the composer at its caret. Doors has
 *           no system clipboard; the composer is where words are used.
 *   RESEND  an outgoing direct message that went unacknowledged, or one the
 *           service restarted under (rift_model_can_resend): sent again, its
 *           own text, as the service defines a resend.
 *   CLOSE   the bar goes.
 *
 * Keys while it is open: UP and DOWN move to the message before or after,
 * LEFT and RIGHT to an action, ENTER does it, ESC closes the bar and gives
 * the composer the keys back.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_MSGACT_H
#define RIFT_MSGACT_H

#include "rift_app.h"

struct rift_msgact;

/* The bar, hidden, at the end of parent's children. */
struct rift_msgact *rift_msgact_create(struct rift_app *app, lv_obj_t *parent);
void rift_msgact_destroy(struct rift_msgact *b);

/* Open on the message with this id in conversation conv. by_key puts the
 * keys on its first action. */
void rift_msgact_open(struct rift_msgact *b, int64_t id, const char *conv, int by_key);
void rift_msgact_close(struct rift_msgact *b);
/* The message it is open on, or 0 when it is closed. */
int64_t rift_msgact_id(const struct rift_msgact *b);
int rift_msgact_by_key(const struct rift_msgact *b);

/* Follow the model: closed when the conversation is another one or the
 * message has left the window; the actions shown are the ones it has now. */
void rift_msgact_refresh(struct rift_msgact *b, const char *conv);

/* A key while the bar is open; ids are the thread's messages, oldest first.
 * Returns 1 when the key was the bar's. */
int rift_msgact_key(struct rift_msgact *b, uint32_t key, const int64_t *ids, int count);

/* A channel message's body, drawn as a reply when it is one: the mention and
 * quotation (rift_reply_parse) set on quote - a caption label - as
 * "(U+21B3) Name?: quotation" in the quoted name's identity accent, and
 * the answer returned for the body label. Not a reply: quote is hidden and
 * body is returned as it came. *ident_on / *ident are the label's accent. */
const char *rift_msgact_quote(lv_obj_t *quote, const char *body, int *ident_on, uint32_t *ident);

/* For the tests: the button of an action, NULL when it is not shown. */
#define RIFT_MSGACT_REPLY 0
#define RIFT_MSGACT_COPY 1
#define RIFT_MSGACT_RESEND 2
#define RIFT_MSGACT_CLOSE 3
lv_obj_t *rift_msgact_button(const struct rift_msgact *b, int which);

#endif

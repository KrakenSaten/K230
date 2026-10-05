/*
 * RIFT: a reply to a channel message, as ordinary message text.
 *
 * MeshCore has no reply field. A PAYLOAD_TYPE_GRP_TXT payload is a
 * timestamp, a flags byte and "<sender>: <text>" (BaseChatMesh.cpp,
 * sendGroupMessage), and no client in the pinned tree - the companion
 * firmware, its ui-rift, the repeater - carries a reference to another
 * message in it. So a reply is text, written in a shape every MeshCore
 * client already reads:
 *
 *     @[Anna] "Are you coming up?" Yes, in 10 minutes
 *
 * "@[name]" is the mention the MeshCore app writes when a channel member is
 * tagged (its release notes for 1.27.0 add tagging, and node names lose the
 * square brackets for it - DOCUMENTED there, not in the pinned source); the
 * quoted part is a short preview of what is being answered. A client that
 * knows neither reads a name, a quotation and an answer. RIFT reads the same
 * text back and draws the quotation as a line of its own above the answer
 * (rift_reply_parse). Nothing binary is added, and nothing is sent that the
 * reader has not seen: the prefix is put into the composer, where it can be
 * read, changed or deleted before SEND.
 *
 * Plain C, no LVGL, no I/O: tested by tests/rift_format_test.c.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RIFT_REPLY_H
#define RIFT_REPLY_H

#include <stddef.h>

#include "rift_model.h"

/* The quotation is at most this many bytes of the message answered, cut at
 * a word where one is near and never inside a character or an emoji
 * sequence; a cut one ends in U+2026. The name is what MeshCore allows one
 * to be (31 bytes). A whole prefix therefore stays under 80 bytes, which
 * leaves a channel's text limit most of its room for the answer. */
#define RIFT_REPLY_QUOTE_MAX 32
#define RIFT_REPLY_NAME_MAX 32
#define RIFT_REPLY_PREFIX_MAX (RIFT_REPLY_NAME_MAX + RIFT_REPLY_QUOTE_MAX + 16)

/* Whether a message can be replied to: an incoming channel message that
 * names its sender. A direct thread is already a reply to one person, and a
 * line with no claimed name has nobody to address. */
int rift_reply_possible(const struct rift_message *msg);

/* The prefix of a reply to msg - '@[' name '] "' quote '" ' - into out.
 * The name is the sender's claimed name with any '[' or ']' in it dropped
 * (they would end the mention early); the quotation is the message's body
 * (rift_msg_body), with newlines and tabs as spaces and any '"' as '\''.
 * Returns the bytes written, 0 when msg cannot be replied to. */
size_t rift_reply_prefix(const struct rift_message *msg, char *out, size_t out_len);

/* A body that starts with a mention, read back. */
struct rift_reply {
    char name[RIFT_REPLY_NAME_MAX];
    char quote[RIFT_REPLY_QUOTE_MAX * 2]; /* "" when there is none */
    const char *rest; /* the answer: points into the body that was parsed */
};

/* Whether body starts with "@[name] " (a name of 1 to 31 bytes holding no
 * ']'), optionally followed by '"' quote '" '; on 1, *out holds the name,
 * the quotation and where the answer starts. Anything else is 0, and the
 * body is shown as it came. */
int rift_reply_parse(const char *body, struct rift_reply *out);

#endif

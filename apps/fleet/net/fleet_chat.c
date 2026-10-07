/*
 * PocketFleet multiplayer chat lines. See fleet_chat.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fleet_chat.h"

#include <string.h>

void fleet_chat_reset(struct fleet_chat *c, uint8_t first_id)
{
    memset(c, 0, sizeof(*c));
    c->next_id = first_id;
}

static int waiting(const struct fleet_chat_line *l)
{
    return l->mine && (l->state == FLEET_CHAT_QUEUED || l->state == FLEET_CHAT_SENDING);
}

int fleet_chat_pending(const struct fleet_chat *c)
{
    int n = 0;
    int i;

    for (i = 0; i < c->count; i++) {
        n += waiting(&c->line[i]);
    }
    return n;
}

/* Room for one more line: when the history is full the oldest line that is
 * settled goes. A line still owed a receipt is never the one dropped, and
 * there are never more of those than FLEET_CHAT_OUTGOING, so one is always
 * found. Returns the slot, or NULL. */
static struct fleet_chat_line *append(struct fleet_chat *c)
{
    struct fleet_chat_line *l;
    int i;

    if (c->count == FLEET_CHAT_HISTORY) {
        for (i = 0; i < c->count && waiting(&c->line[i]); i++) {
        }
        if (i == c->count) {
            return NULL;
        }
        memmove(&c->line[i], &c->line[i + 1], (size_t)(c->count - i - 1) * sizeof(c->line[0]));
        c->count--;
    }
    l = &c->line[c->count++];
    memset(l, 0, sizeof(*l));
    return l;
}

int fleet_chat_add_mine(struct fleet_chat *c, const char *text)
{
    struct fleet_chat_line *l;
    size_t start = 0;
    size_t end;

    if (!text) {
        return -1;
    }
    end = strlen(text);
    while (start < end && (text[start] == ' ' || text[start] == '\t')) {
        start++;
    }
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        end--;
    }
    if (!fleet_chat_text_ok((const uint8_t *)text + start, end - start)) {
        return -1;
    }
    if (fleet_chat_pending(c) >= FLEET_CHAT_OUTGOING) {
        return -2;
    }
    l = append(c);
    if (!l) {
        return -2;
    }
    l->mine = 1;
    l->state = FLEET_CHAT_QUEUED;
    l->id = c->next_id++;
    l->len = (uint8_t)(end - start);
    memcpy(l->text, text + start, l->len);
    l->text[l->len] = '\0';
    l->check = fleet_chat_check(l->id, (const uint8_t *)l->text, l->len);
    return 0;
}

int fleet_chat_add_theirs(struct fleet_chat *c, uint8_t id, const uint8_t *text, size_t n)
{
    struct fleet_chat_line *l;
    uint16_t check;
    int i;

    if (!fleet_chat_text_ok(text, n)) {
        return -1;
    }
    check = fleet_chat_check(id, text, n);
    /* A copy: the same id with the same words. A new run of the peer's app
     * may reuse an id, but not with the same line, so it is not mistaken for
     * a copy unless it says exactly what the copy said. */
    for (i = 0; i < c->seen_count; i++) {
        if (c->seen_id[i] == id && c->seen_check[i] == check) {
            return 0;
        }
    }
    l = append(c);
    if (!l) {
        return 0;
    }
    c->seen_id[c->seen_next] = id;
    c->seen_check[c->seen_next] = check;
    c->seen_next = (uint8_t)((c->seen_next + 1) % FLEET_CHAT_SEEN);
    if (c->seen_count < FLEET_CHAT_SEEN) {
        c->seen_count++;
    }
    l->mine = 0;
    l->state = FLEET_CHAT_RECEIVED;
    l->id = id;
    l->len = (uint8_t)n;
    memcpy(l->text, text, n);
    l->text[n] = '\0';
    l->check = check;
    if (c->unread < 255) {
        c->unread++;
    }
    return 1;
}

struct fleet_chat_line *fleet_chat_outgoing(struct fleet_chat *c)
{
    int i;

    for (i = 0; i < c->count; i++) {
        if (waiting(&c->line[i])) {
            return &c->line[i];
        }
    }
    return NULL;
}

int fleet_chat_acked(struct fleet_chat *c, uint8_t id, uint16_t check)
{
    int i;

    for (i = 0; i < c->count; i++) {
        struct fleet_chat_line *l = &c->line[i];

        /* A receipt that comes after we gave up still tells the truth. */
        if (l->mine && l->id == id && l->check == check &&
            (l->state == FLEET_CHAT_SENDING || l->state == FLEET_CHAT_FAILED)) {
            if (l->state == FLEET_CHAT_SENDING) {
                c->tries = 0;
            }
            l->state = FLEET_CHAT_DELIVERED;
            return 1;
        }
    }
    return 0;
}

int fleet_chat_fail_pending(struct fleet_chat *c)
{
    int n = 0;
    int i;

    for (i = 0; i < c->count; i++) {
        if (waiting(&c->line[i])) {
            c->line[i].state = FLEET_CHAT_FAILED;
            n++;
        }
    }
    c->tries = 0;
    return n;
}

void fleet_chat_seen(struct fleet_chat *c)
{
    c->unread = 0;
}

const struct fleet_chat_line *fleet_chat_last(const struct fleet_chat *c)
{
    return c->count ? &c->line[c->count - 1] : NULL;
}

/*
 * RIFT: the emoji the composer's picker offers (rift_emoji_pick.h).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_emoji_pick.h"

#include <stdio.h>
#include <string.h>

/* The owner's list, in the owner's order. */
static const char *const common[RIFT_EMOJI_CELLS] = {
    "\xF0\x9F\x99\x82",         /* U+1F642 slightly smiling face */
    "\xF0\x9F\x91\x8D",         /* U+1F44D thumbs up */
    "\xF0\x9F\x91\x8B",         /* U+1F44B waving hand */
    "\xE2\x9D\xA4\xEF\xB8\x8F", /* U+2764 U+FE0F red heart */
    "\xF0\x9F\x98\x82",         /* U+1F602 tears of joy */
    "\xF0\x9F\x94\xA5",         /* U+1F525 fire */
    "\xE2\x9C\x85",             /* U+2705 check mark button */
    "\xE2\x9D\x8C",             /* U+274C cross mark */
    "\xE2\x9A\xA0\xEF\xB8\x8F", /* U+26A0 U+FE0F warning */
    "\xF0\x9F\x93\xA1",         /* U+1F4E1 satellite antenna */
    "\xF0\x9F\x93\x8D",         /* U+1F4CD round pushpin */
    "\xF0\x9F\x9A\x97",         /* U+1F697 automobile */
    "\xE2\x98\x95",             /* U+2615 hot beverage */
    "\xF0\x9F\x94\xA6",         /* U+1F526 flashlight */
    "\xF0\x9F\x8C\xB2",         /* U+1F332 evergreen tree */
};

static const char *const people[RIFT_EMOJI_CELLS] = {
    "\xF0\x9F\x98\x80", /* U+1F600 grinning face */
    "\xF0\x9F\x98\x8A", /* U+1F60A smiling eyes */
    "\xF0\x9F\x98\x89", /* U+1F609 winking face */
    "\xF0\x9F\x98\x8E", /* U+1F60E sunglasses */
    "\xF0\x9F\xA4\x94", /* U+1F914 thinking face */
    "\xF0\x9F\x98\xAE", /* U+1F62E open mouth */
    "\xF0\x9F\x98\xA2", /* U+1F622 crying face */
    "\xF0\x9F\x98\xA1", /* U+1F621 pouting face */
    "\xF0\x9F\x98\xB4", /* U+1F634 sleeping face */
    "\xF0\x9F\x99\x8F", /* U+1F64F folded hands */
    "\xF0\x9F\x91\x8F", /* U+1F44F clapping hands */
    "\xF0\x9F\x91\x8C", /* U+1F44C OK hand */
    "\xF0\x9F\x92\xAA", /* U+1F4AA flexed biceps */
    "\xF0\x9F\xA4\x9D", /* U+1F91D handshake */
    "\xF0\x9F\x91\x80", /* U+1F440 eyes */
};

static const char *const field[RIFT_EMOJI_CELLS] = {
    "\xF0\x9F\x93\xBB",                 /* U+1F4FB radio */
    "\xF0\x9F\x93\xB6",                 /* U+1F4F6 antenna bars */
    "\xF0\x9F\x94\x8B",                 /* U+1F50B battery */
    "\xF0\x9F\x9B\xB0\xEF\xB8\x8F",     /* U+1F6F0 U+FE0F satellite */
    "\xF0\x9F\xA7\xAD",                 /* U+1F9ED compass */
    "\xF0\x9F\x97\xBA\xEF\xB8\x8F",     /* U+1F5FA U+FE0F world map */
    "\xE2\x9B\xBA",                     /* U+26FA tent */
    "\xF0\x9F\x8F\x94\xEF\xB8\x8F",     /* U+1F3D4 U+FE0F snow-capped mountain */
    "\xF0\x9F\x8C\xA7\xEF\xB8\x8F",     /* U+1F327 U+FE0F cloud with rain */
    "\xE2\x9D\x84\xEF\xB8\x8F",         /* U+2744 U+FE0F snowflake */
    "\xE2\x98\x80\xEF\xB8\x8F",         /* U+2600 U+FE0F sun */
    "\xF0\x9F\x8C\x99",                 /* U+1F319 crescent moon */
    "\xE2\x8F\xB0",                     /* U+23F0 alarm clock */
    "\xF0\x9F\x86\x98",                 /* U+1F198 SOS button */
    "\xF0\x9F\x8F\xA0",                 /* U+1F3E0 house */
};

const struct rift_emoji_group rift_emoji_groups[RIFT_EMOJI_GROUPS] = {
    { "RECENT / COMMON", "\xE2\xAD\x90" /* U+2B50 star */, common, RIFT_EMOJI_CELLS },
    { "PEOPLE / REACTIONS", "\xF0\x9F\x99\x82", people, RIFT_EMOJI_CELLS },
    { "RADIO / FIELD", "\xF0\x9F\x93\xA1", field, RIFT_EMOJI_CELLS },
};

const char *rift_emoji_pick_find(const char *utf8)
{
    unsigned g;
    unsigned i;

    if (!utf8 || !utf8[0]) {
        return NULL;
    }
    for (g = 0; g < RIFT_EMOJI_GROUPS; g++) {
        for (i = 0; i < rift_emoji_groups[g].count; i++) {
            if (strcmp(rift_emoji_groups[g].items[i], utf8) == 0) {
                return rift_emoji_groups[g].items[i];
            }
        }
    }
    return NULL;
}

static int holds(const char *const *list, unsigned n, const char *emoji)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        if (list[i] == emoji) {
            return 1;
        }
    }
    return 0;
}

unsigned rift_emoji_group_items(unsigned g, const char *const *recent, unsigned n_recent,
                                const char **out)
{
    const struct rift_emoji_group *grp;
    unsigned n = 0;
    unsigned i;

    if (!out || g >= RIFT_EMOJI_GROUPS) {
        return 0;
    }
    grp = &rift_emoji_groups[g];
    if (g == 0 && recent) {
        for (i = 0; i < n_recent && n < RIFT_EMOJI_RECENT_MAX; i++) {
            /* Only the table's own strings, so the check below is by
             * pointer and a recent emoji is never shown twice. */
            const char *e = rift_emoji_pick_find(recent[i]);

            if (e && !holds(out, n, e)) {
                out[n++] = e;
            }
        }
    }
    for (i = 0; i < grp->count && n < RIFT_EMOJI_CELLS; i++) {
        if (!holds(out, n, grp->items[i])) {
            out[n++] = grp->items[i];
        }
    }
    return n;
}

unsigned rift_emoji_recent_parse(const char *text, const char **out)
{
    unsigned n = 0;

    if (!text || !out) {
        return 0;
    }
    while (*text && n < RIFT_EMOJI_RECENT_MAX) {
        char token[RIFT_EMOJI_PICK_LEN];
        size_t len;
        const char *e;

        text += strspn(text, " ");
        len = strcspn(text, " ");
        if (len == 0) {
            break;
        }
        if (len < sizeof(token)) {
            memcpy(token, text, len);
            token[len] = '\0';
            e = rift_emoji_pick_find(token);
            if (e && !holds(out, n, e)) {
                out[n++] = e;
            }
        }
        text += len;
    }
    return n;
}

unsigned rift_emoji_recent_push(const char **recent, unsigned n, const char *emoji)
{
    const char *e = rift_emoji_pick_find(emoji);
    unsigned i;

    if (!recent || !e) {
        return n;
    }
    if (n > RIFT_EMOJI_RECENT_MAX) {
        n = RIFT_EMOJI_RECENT_MAX;
    }
    for (i = 0; i < n && recent[i] != e; i++) {
    }
    if (i == n && n == RIFT_EMOJI_RECENT_MAX) {
        i = n - 1; /* full: the oldest goes */
    } else if (i == n) {
        n++;
    }
    memmove(&recent[1], &recent[0], sizeof(recent[0]) * i);
    recent[0] = e;
    return n;
}

int rift_emoji_recent_format(const char *const *recent, unsigned n, char *out, size_t out_len)
{
    size_t at = 0;
    unsigned i;

    if (!out || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
    for (i = 0; recent && i < n; i++) {
        int w = snprintf(out + at, out_len - at, "%s%s", i ? " " : "", recent[i]);

        if (w < 0 || (size_t)w >= out_len - at) {
            out[0] = '\0';
            return -1;
        }
        at += (size_t)w;
    }
    return (int)at;
}

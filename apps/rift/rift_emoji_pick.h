/*
 * RIFT: the emoji the composer's picker offers (ui/rift_emoji_picker.h).
 *
 * A short, fixed list in three groups, not an emoji database: what a person
 * on a mesh radio says most - a reaction, a yes or a no, a warning, where
 * and what - with the common ones first. Each entry is the emoji's
 * fully-qualified UTF-8 (a variation selector where Unicode wants one, so
 * other MeshCore clients draw it as an emoji too), and none is toned: RIFT
 * makes no skin-tone variants (owner rule, rift_emoji.h).
 *
 * The first group also holds what this reader picked lately, first. The
 * recent list is kept in RIFT's preferences file (rift_store.h) as the
 * emoji themselves, separated by spaces; what it holds that is not in this
 * table is not shown and not kept.
 *
 * Plain C, no LVGL: tested on its own (tests/rift_emoji_ui_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_EMOJI_PICK_H
#define RIFT_EMOJI_PICK_H

#include <stddef.h>

#define RIFT_EMOJI_GROUPS 3
/* A group fills the picker's grid: five across, three down. */
#define RIFT_EMOJI_COLS 5
#define RIFT_EMOJI_CELLS 15
/* Recently picked emoji kept, newest first: one row of the grid. */
#define RIFT_EMOJI_RECENT_MAX RIFT_EMOJI_COLS
/* The longest entry in bytes, its NUL included. */
#define RIFT_EMOJI_PICK_LEN 8

struct rift_emoji_group {
    const char *title; /* upper case, as RIFT's captions are */
    const char *icon;  /* the group's tab: one of the table's emoji */
    const char *const *items;
    unsigned count;    /* RIFT_EMOJI_CELLS */
};

extern const struct rift_emoji_group rift_emoji_groups[RIFT_EMOJI_GROUPS];

/* The table's own string for utf8 (exactly one entry, compared whole), or
 * NULL when utf8 is not one the picker offers. */
const char *rift_emoji_pick_find(const char *utf8);

/* What group g shows, in order, into out[RIFT_EMOJI_CELLS]; returns how
 * many. Group 0 is the recent emoji first, then the common ones not already
 * shown, cut at a full grid. recent may be NULL when n_recent is 0. */
unsigned rift_emoji_group_items(unsigned g, const char *const *recent, unsigned n_recent,
                                const char **out);

/* The recent list as the preferences file holds it, into
 * out[RIFT_EMOJI_RECENT_MAX] as the table's own strings: unknown entries
 * and repeats are dropped. Returns how many. */
unsigned rift_emoji_recent_parse(const char *text, const char **out);

/* Put emoji (one of the table's) first in recent, dropping an earlier copy
 * and, when full, the oldest. Returns the new count. */
unsigned rift_emoji_recent_push(const char **recent, unsigned n, const char *emoji);

/* The list as the preferences file holds it. Returns the length, or -1 when
 * out is too small. */
int rift_emoji_recent_format(const char *const *recent, unsigned n, char *out, size_t out_len);

#endif

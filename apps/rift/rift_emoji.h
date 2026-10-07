/*
 * RIFT: emoji in message text, as one image each.
 *
 * LVGL draws one code point at a time and composes nothing, so what Unicode
 * spells as a sequence - a flag (two regional indicators), a keycap, a family
 * joined with U+200D, a tag flag - is folded here, for display only, into one
 * private-use code point (RIFT_EMOJI_PUA + its index in rift_emoji_seqs[]),
 * which RIFT's colour emoji font (ui/rift_emoji_font.h) draws as the Noto
 * Color Emoji artwork for the whole sequence. The table is generated from
 * that artwork (tools/design/gen_rift_emoji.js), so a sequence is folded
 * exactly when there is an image for it.
 *
 * Plain C, no LVGL: the folding is tested on its own (tests/rift_format_test).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_EMOJI_H
#define RIFT_EMOJI_H

#include <stddef.h>
#include <stdint.h>

/* The first private-use code point a folded sequence becomes. Remote text
 * that already holds one of these (planes 15 and 16) is shown as U+FFFD, so
 * nobody can make RIFT draw an emoji they did not send. */
#define RIFT_EMOJI_PUA 0xF0000u

/* The longest sequence in the table, in code points. */
#define RIFT_EMOJI_SEQ_MAX 8

struct rift_emoji_seq {
    uint16_t at;  /* first code point in rift_emoji_seq_cps[] */
    uint8_t len;  /* code points, 2..RIFT_EMOJI_SEQ_MAX */
};

/* Sorted by their code points, compared one by one (generated). */
extern const uint32_t rift_emoji_seq_cps[];
extern const struct rift_emoji_seq rift_emoji_seqs[];
extern const unsigned rift_emoji_seq_count;

/* Message text as RIFT draws it in a message body or preview: the variation
 * selectors U+FE0E/U+FE0F and the skin-tone modifiers U+1F3FB-1F3FF are
 * dropped (a toned emoji shows its base: no toned artwork is made), then the
 * longest sequence with an image at each place is folded into its private
 * code point. What is left of a sequence with no image is its parts: the
 * joiner, the keycap mark and stray tag characters are dropped, a lone
 * regional indicator stays (it has an image of its own). A byte that starts
 * no UTF-8 character, and a private-use code point of planes 15 and 16,
 * become U+FFFD. Everything else is copied as it is. The result is cut on a
 * character boundary if out is short. Presentation only: what is stored,
 * sent and counted is the text as it came. Returns the bytes written. */
size_t rift_emoji_fold(const char *in, char *out, size_t out_len);

#endif

/*
 * RIFT: whether the next letter typed into a composer starts a sentence.
 *
 * The composer offers a capital there by engaging the touch keyboard's
 * one-shot Shift (rift_focus.c); this only answers the question, from the
 * text before the caret, so nothing is ever rewritten and nothing is
 * remembered between two answers. A sentence starts:
 *
 *   - at the start of the message, and
 *   - after a sentence end - ".", "!" or "?" (any run of them, "?!" and
 *     "!!!" included) - followed by white space.
 *
 * What lies between that start and the caret, if it is not a letter, does
 * not use the capital up: a new message that opens with an emoji, a number
 * or a quotation mark still wants its first letter capitalised. A letter
 * does: after it, the next sentence end is waited for again.
 *
 * Not sentence ends: a dot inside a word, a number, an address or a URL
 * ("3.5", "example.com", "a@b.no" - no white space follows it), and an
 * ellipsis ("..." or U+2026), which goes on rather than ends.
 *
 * Letters are the Latin ones: ASCII and Latin-1 to Latin Extended-B
 * (U+00C0-U+024F but the multiplication and division signs), so the
 * Norwegian æ, ø and å count, upper and lower case alike.
 *
 * Plain C, no LVGL: tested on its own (tests/rift_autocap_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_AUTOCAP_H
#define RIFT_AUTOCAP_H

#include <stdint.h>

/* 1 when the next letter typed at caret (in characters, as LVGL's text area
 * counts its cursor) starts a sentence of text (UTF-8, NULL as ""), else 0.
 * A caret past the end is at the end. */
int rift_autocap_wanted(const char *text, uint32_t caret);

#endif

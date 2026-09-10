/*
 * PocketNotes text rules: what a note's title is, whether it counts as
 * empty, and whether its bytes are text at all.
 *
 * No LVGL and no I/O, so the rules that decide what the list shows and what
 * gets stored can be tested without a display or a filesystem
 * (tests/notes_lint.sh).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETNOTES_VIEW_H
#define POCKETNOTES_VIEW_H

#include <stddef.h>

/* Shown for a note whose text is blank, which only happens for a note being
 * written; a blank note is deleted rather than stored. */
#define NOTES_UNTITLED "Untitled"

/* How much of a title the list keeps. A title is derived text, so its bound
 * lives with the rule that derives it. */
#define NOTES_TITLE_MAX 64

/* The title is the note's first non-blank line, with surrounding whitespace
 * removed and truncated to fit - never in the middle of a UTF-8 character,
 * so a truncated title is still text. Always writes something. */
void notes_title_from_text(const char *text, char *out, size_t out_len);

/* What the list shows for a note that could not be read. */
void notes_title_unreadable(char *out, size_t out_len);

/* True when the text holds nothing but whitespace. Such a note is not worth
 * a file: the editor deletes it instead of saving it. */
int notes_text_is_blank(const char *text);

/* True when the whole string is well-formed UTF-8. A note is text; bytes
 * that are not are refused rather than shown as mojibake and saved back. */
int notes_text_is_utf8(const char *text);

#endif
